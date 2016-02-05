#include "CCVideoSprite.h"
#include "utils.h"

NS_CC_BEGIN
using namespace std;


// cocos2d and ffmpeg both use PixelFormat, but ffmpeg uses #define, which messes with cocos2d.
// Thus undefine, as we dont need the ffmpeg's version here.
#ifdef PixelFormat
#undef PixelFormat
#endif

const int MAX_DECODE_QUEUE_SIZE = 20;

double pts2Sec(int64_t pts);


CCVideoSprite* CCVideoSprite::create(const std::string &pFileName) {
    return CCVideoSprite::create(pFileName, Size::ZERO);
}

CCVideoSprite* CCVideoSprite::create(const std::string &pFileName,
                                     const Size &pSize) {
    CCVideoSprite *sprite = new CCVideoSprite();
    
    if(sprite && sprite->init(pFileName, pSize)) {
        sprite->autorelease();
        return sprite;
    }
    
    CC_SAFE_DELETE(sprite);
    
    return nullptr;
}

CCVideoSprite::CCVideoSprite() {
    srcWidth = dstHeight = -1;
    dstWidth = dstHeight = -1;
    frameRate = 1;
    currentFrame = 0;
    videoEndCallback = nullptr;
    
    
    formatContext = NULL;
    codecContext = NULL;
    videoStreamIdx = -1;
    
    swsContext = NULL;
    
    totalLoops = -1;
    
    pthread_mutex_init(&decoderMutex, NULL);
}

CCVideoSprite::~CCVideoSprite() {
    
    stopDecodingThread();
    
    removeOldPictures(true);
    
    if(codecContext) avcodec_free_context(&codecContext);
    if(formatContext) avformat_close_input(&formatContext);
}

bool CCVideoSprite::init(const std::string &pFileName, const Size &pSize) {
    
    videoFileName = pFileName;
    
    int err;
    
    if((err = avformat_open_input(&formatContext, videoFileName.c_str(),
                                  NULL, NULL)) != 0) {
        CCLOG("Failed to open video input %s, error- %i", videoFileName.c_str(), err);
        return false;
    }
    
    if((err = avformat_find_stream_info(formatContext, NULL)) < 0) {
        CCLOG("Corrupt/unknown video input %s, error- %i", videoFileName.c_str(), err);
        return false;
    }
    
    av_dump_format(formatContext, 0, videoFileName.c_str(), 0);
    
    int64_t duration = formatContext->duration;
    double durationSecs = (double)duration / AV_TIME_BASE;
    
    CCLOG("Duration : %f secs", durationSecs);
    
    AVStream *videoStream = NULL;
    const AVCodec *videoCodec = NULL;
    
    for(int i = 0; i < formatContext->nb_streams; i++) {
        if(formatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIdx = i;
            videoStream = formatContext->streams[i];
            videoCodec = avcodec_find_decoder(videoStream->codec->codec_id);
            break;
        }
    }
    
    if(videoStreamIdx == -1 || !videoStream || !videoCodec) {
        CCLOG("No video stream found in %s!", videoFileName.c_str());
        avformat_close_input(&formatContext);
        return false;
    }
    
    CCLOG("Avg fps: %f", av_q2d(videoStream->avg_frame_rate));
    
    frameRate = av_q2d(videoStream->avg_frame_rate);
    if(frameRate < 1 || frameRate > 100) {
        CCLOG("Unexpected frame rate value: %f, using 30", frameRate);
    }
    
    srcWidth = videoStream->codec->width;
    srcHeight = videoStream->codec->height;
    
    if(srcWidth <= 0 || srcHeight <= 0) {
        CCLOG("Video %s has invalid size: %i : %i!!!", videoFileName.c_str(),
              srcWidth, srcHeight);
        avformat_close_input(&formatContext);
        return false;
    } else {
        CCLOG("video w:h %i:%i", srcWidth, srcHeight);
    }
    
    codecContext = avcodec_alloc_context3(videoCodec);
    
    if(!codecContext) {
        CCLOG("Failed to create video decoder for %s!!!", videoFileName.c_str());
        avformat_close_input(&formatContext);
        return false;
    }
    
    if(avcodec_copy_context(codecContext, videoStream->codec)) {
        CCLOG("Failed to copy codec context for %s", videoFileName.c_str());
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }

    if(avcodec_open2(codecContext, videoCodec, NULL)) {
        CCLOG("Failed to open codec for %s", videoFileName.c_str());
        avcodec_free_context(&codecContext);
        avformat_close_input(&formatContext);
        return false;
    }
    
    CCLOG("Video sprite created for: %s", videoFileName.c_str());
    
    if(pSize.equals(Size::ZERO)) {
        dstWidth = srcWidth;
        dstHeight = srcHeight;
    } else {
//        dstWidth = pSize.width;
//        dstHeight = pSize.height;

        dstWidth = srcWidth;
        dstHeight = srcHeight;

    }

    CCLOG("Video sprite size: %i:%i -> %i:%i", srcWidth, srcHeight, dstWidth, dstHeight);
    
    // init with grey rectangle
    // use RGB not RGBA or else, it needs to be set to 255 and memset cannot be used
    int buflen = dstHeight * dstWidth * 3;
    unsigned char *buf = (unsigned char*) malloc(buflen);
    memset(buf, 0x7f, buflen);
    
    Texture2D *texture = new Texture2D();
    texture->initWithData(buf, buflen, cocos2d::Texture2D::PixelFormat::RGB888,
                          dstWidth, dstHeight, Size(dstWidth, dstHeight));
    free(buf);
    
    
    // initWithTexture assumes ownership of the texture object
    initWithTexture(texture);
    CC_SAFE_RELEASE(texture);
    
    
    
    this->setContentSize(Size(dstWidth, dstHeight));

//    if(!pSize.equals(Size::ZERO)) this->setScale((float) srcWidth/ (float)dstWidth, (float)srcHeight / (float) dstHeight);

    
    startDecodingThread();
    
    return true;
}

void CCVideoSprite::startDecodingThread() {
    
    CCLOG("starting decoder thread...");
    
    terminateDecoderThread = false;
    decoderThreadRunning = true;
    
    void* (*f)(void*) = (void* (*) (void*)) CCVideoSprite::decoderFunc;
    if(pthread_create(&decoderThread, NULL, f, this)) {
        CCLOG("Failed to create decoder thread: %s", strerror(errno));
        decoderThreadRunning = false;
        return;
    }
    
    CCLOG("decoder thread started");
    
}

void CCVideoSprite::stopDecodingThread() {
    if(decoderThreadRunning) {
        terminateDecoderThread = true;
        void *retVal;
        CCLOG("stopDecodingThread- wait for decoder to finish");
        pthread_join(decoderThread, &retVal);
        CCLOG("stopDecodingThread finished");
    } else {
        CCLOG("decoder already stopped");
    }
}

void CCVideoSprite::addDecodedPicture(double pts, AVPicture* picture) {
    pthread_mutex_lock(&decoderMutex);
    
     if(decodedPictures.find(pts) == decodedPictures.end()) {
         decodedPictures[pts] = picture;
         pthread_mutex_unlock(&decoderMutex);
     } else {
         pthread_mutex_unlock(&decoderMutex); // release lock as soon as possible
         
         // There is picture with such pts value already present... strange.
         CCLOG("Picture PTS (%f) is not unique!!!", pts);
         avpicture_free(picture);
         free(picture);
     }
}

void CCVideoSprite::removeOldPictures(bool removeAll) {
    
    int removed = 0;
    
    pthread_mutex_lock(&decoderMutex);
    while(1) {
        
        if(decodedPictures.empty()) break;
        
        auto i = decodedPictures.begin();
        
        if(i->first <= currentPts || removeAll) {
            AVPicture *picture = i->second;
            decodedPictures.erase(i);
            avpicture_free(picture);
            free(picture);
            removed++;
        } else {
            break; // All entries are only newer
        }
    }
    pthread_mutex_unlock(&decoderMutex);
    
    if(removeAll) {
        //CCLOG("Removed all %i pictures", removed);
    } else {
        //CCLOG("Removed %i pictures <= %lli", removed, rpts);
    }
}



int CCVideoSprite::play(double startTime, int loops) {
    CCLOG("play video!");
    
    totalLoops = -1;
    
    if(loops >= 0) totalLoops = loops;
    
//    if(loop) {
//        setVideoEndCallback(CCVideoSprite::restartVideoIndef);
//    }
    
    
    currentPts = 0;
    
    playStartTime = startTime;

    currentTime = playerutils::getCurrentTime();
    currentFrameTime = currentTime;
    
    schedule(schedule_selector(CCVideoSprite::update), 0.033);
    
    CCLOG("playback (%lu) started in: %f", (unsigned long) this, currentTime);
    return 0;
}

int CCVideoSprite::stop() {
    CCLOG("stop video!");
    unscheduleAllCallbacks();
    return 0;
}

int CCVideoSprite::seek(float seekTime) {
    CCLOG("seek video to: %f", seekTime);
    
    
    stop();
    stopDecodingThread();
    removeOldPictures(true);
    
    avcodec_flush_buffers(codecContext);
    
    // Fixme- this is seek to 0 frame.
    // Must be replaced with real seek
    
    //av_seek_frame(formatContext, -1, 0, AVSEEK_FLAG_FRAME);
    
    int64_t pos = (seekTime - 0.5) * AV_TIME_BASE;
    
    av_seek_frame(formatContext, -1, pos, 0);
    
    playStartTime = playerutils::getCurrentTime() - seekTime;
    currentTime = playerutils::getCurrentTime();
    currentFrameTime = currentTime;
    
    decodeVideo();
    
    startDecodingThread();
    schedule(schedule_selector(CCVideoSprite::update), 0.033);

    
    return 0;
}

int CCVideoSprite::setVideoEndCallback(std::function<void (CCVideoSprite *)> pFunc) {
    videoEndCallback = pFunc;
    return 0;
}

void CCVideoSprite::update(float dt) {
    currentTime = playerutils::getCurrentTime();
//    CCLOG("Update dt: %f, dt (%lu): %f, currPts: %f", dt, (unsigned long) this, currentTime - playStartTime, currentPts);
    displayVideoPicture();
   
}


void CCVideoSprite::displayVideoPicture() {
    
    double secondsPerFrame = 1.0 / frameRate;
    double secondsSinceLastUpdate = currentTime - currentFrameTime;
    
    if(secondsSinceLastUpdate < secondsPerFrame) {
        CCLOG("Frame should not be switched yet (%f / %f)", secondsSinceLastUpdate, secondsPerFrame);
        return;
    }
    
    // Remove frames older than current frame
    //removeOldPictures();
    
    pthread_mutex_lock(&decoderMutex);
    
    if(decodedPictures.empty()) {
        pthread_mutex_unlock(&decoderMutex);
        CCLOG("No decoded pictures to display!!!");
        
        //
        // if decoder for some reason has stopped (eof, corrupt file, ...)
        //   and no more pictures are to display, then playback has completed
        if(!decoderThreadRunning) {
            stop();
            
            if(totalLoops != 0) {
                CCLOG("loops remaining: %i", totalLoops);
                seek(0);
                play(playerutils::getCurrentTime(), totalLoops - 1);
                return;
            }
            
            if(videoEndCallback != nullptr) {
                CCLOG("Calling video end callback");
                videoEndCallback(this);
                CCLOG("Video end callback completed");
            }
        }
        return;
    }

    // Calculate current time
    
    double newPts = currentTime - playStartTime;
    
    auto i = decodedPictures.begin();
    double largestPts = 0.0;
    
    AVPicture *picture = nullptr;
    double pts = 0.0;
    
    //CCLOG("queue size: %i, current pts: %f, newpts: %f, video lag: %f", (int)decodedPictures.size(), currentPts, newPts, newPts - currentPts);
    while(i != decodedPictures.end()) {
        largestPts = i->first;
        
        if(newPts >= largestPts) {
            picture = i->second;
            pts = largestPts;
            //CCLOG("    skip: %f", largestPts);
            ++i;
            continue;
        } else {
            //CCLOG("    use: %f (%f)", pts, largestPts);
            break;
        }
    }
    
    pthread_mutex_unlock(&decoderMutex);

    if(!picture) {
        //CCLOG("no pictures found");
        return;
    }
    
    // Must display newer picture
    if(pts < currentPts) {
        CCLOG("no newer pictures found queue");
        return;
    } else if(pts == currentPts) {
        CCLOG("correct frame already displayed");
        return;
    }
    
    
    currentPts = pts;
    
    Texture2D *texture = new Texture2D();
    
    texture->initWithData(picture->data[0], dstWidth * dstHeight * 4,
                          Texture2D::PixelFormat::RGBA8888,
                          dstWidth, dstHeight,
                          Size(dstWidth, dstHeight));
    
    
    //CCLOG("display picture: %f", pts);
    setTexture(texture);
    
    CC_SAFE_RELEASE(texture);
    
    removeOldPictures();
    
    currentFrameTime += secondsPerFrame;
}

void CCVideoSprite::decoderFunc(void *spritePtr) {
    
    CCLOG("Decoder started...");
    
    CCVideoSprite *sprite = (CCVideoSprite*) spritePtr;
    
    while(!sprite->terminateDecoderThread) {
        if(sprite->decodeVideo() < 0) {
            CCLOG("decodeVideo returned error- stopping playback");
            break;
        }
        //CCLOG("decoded picture queue: %i", (int) sprite->decodedPictures.size());
        usleep(sprite->decodedPictures.size() > 15 ? 100 : 0);
    }
    
    
    CCLOG("Decoder finished");
    
    sprite->decoderThreadRunning = false;
    
    pthread_exit(NULL);
}

int CCVideoSprite::decodeVideo() {
    
    int gotPicture = 0;
    
    AVPacket packet;
    int readFrames = 0;
    packet.buf = NULL;
    
    if(decodedPictures.size() >= MAX_DECODE_QUEUE_SIZE) {
        //CCLOG("Decoded picture queue full, skip decode");
        return 1;
    }
    
    int avreaderr = 0;
    
    while(readFrames < 10
          && decodedPictures.size() < MAX_DECODE_QUEUE_SIZE
          && ((avreaderr = av_read_frame(formatContext, &packet)) >= 0)) {

        if(packet.stream_index == videoStreamIdx) {
            
            AVFrame *decodedFrame = av_frame_alloc();
            
            if(!decodedFrame) {
                CCLOG("Failed to allocate video frame!");
                return -1;
            }
            
            int bytesDecoded = avcodec_decode_video2(codecContext,
                                                     decodedFrame,
                                                     &gotPicture,
                                                     &packet);
            
            if(bytesDecoded > 0 && gotPicture) {
                
                if(decodedFrame->pict_type == AV_PICTURE_TYPE_NONE) {
                    CCLOG("Decoded frame with picture type none!");
                } else {
                    
                    int64_t tms = av_frame_get_best_effort_timestamp(decodedFrame);
                    
                    AVStream *stream = formatContext->streams[videoStreamIdx];
                    
                    double secs = (tms - stream->start_time) * av_q2d(stream->time_base);
                    
                    //CCLOG("Decoded video packet pts/dtstms/: %lli/%lli/%lli, secs: %f",
                    //      packet.pts, packet.dts, tms, secs);

                    

                    
                    swsContext = sws_getCachedContext(swsContext,
                                                      codecContext->width,
                                                      codecContext->height,
                                                      codecContext->pix_fmt,
                                                      dstWidth,
                                                      dstHeight,
                                                      PIX_FMT_RGBA,
                                                      /*SWS_FAST_BILINEAR*/0,
                                                      NULL, NULL, NULL);
                    
                    if(!swsContext) {
                        CCLOG("Failed to get scale context!!!");
                        av_frame_free(&decodedFrame);
                        av_free_packet(&packet);
                        return -1;
                    }
                    
                    AVPicture *picture = (AVPicture*) malloc(sizeof(AVPicture));
                    
                    if(avpicture_alloc(picture, PIX_FMT_RGBA, dstWidth, dstHeight)) {
                        CCLOG("Failed to alloc picture!!!");
                        av_frame_free(&decodedFrame);
                        av_free_packet(&packet);
                        return -1;
                    }
                    
                    sws_scale(swsContext, decodedFrame->data, decodedFrame->linesize,
                              0, srcHeight, picture->data, picture->linesize);
                    
                    addDecodedPicture(secs, picture);
                    
                    // picture must not be freed, it's done either by addDecodedPicture
                    //     or removeOldPictures
                    
                    readFrames = 3; // video frame decoded, stop decoding
                }
                
                
                
            } else {
                CCLOG("Failed to decode video frame!!!");
            }
            
            av_frame_free(&decodedFrame);
            
        }
        
        av_free_packet(&packet);
        packet.buf = NULL;
        
        
//        if(gotPicture) {
//            break;
//        }

        
        readFrames++;
    }
    
    if(avreaderr < 0) {
        CCLOG("Failed to read stream: %i", avreaderr);
        return avreaderr;
    }
    
    return 0;
}

void CCVideoSprite::restartVideoIndef(cocos2d::CCVideoSprite *sprite) {
    sprite->seek(0);
    sprite->play(playerutils::getCurrentTime());
}


double pts2Sec(int64_t pts) {
    return (double) pts / AV_TIME_BASE;
}



NS_CC_END

