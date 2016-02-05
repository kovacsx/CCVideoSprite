#ifndef __CCVIDEOSPRITE_H__
#define __CCVIDEOSPRITE_H__

#include "cocos2d.h"
#include <string>
#include <pthread.h>
#include <unistd.h>

extern "C" {
    
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
//#include "libavutil/frame.h"
    
}

NS_CC_BEGIN

class CCVideoSprite : public Sprite
{
public:
    
    static CCVideoSprite* create(const std::string &pFileName);
    static CCVideoSprite* create(const std::string &pFileName, const Size &pSize);
    
    CCVideoSprite();
    virtual ~CCVideoSprite();
    
    bool init(const std::string &pFileName, const Size &pSize = Size::ZERO);
    void update(float dt);
    
    int play(double startTime, int loops = -1); // loops == -1 -> loops forever
    int stop();
    int seek(float seekTime);
    int setVideoEndCallback(std::function<void(CCVideoSprite*)> pFunc);
    
    void addDecodedPicture(double pts, AVPicture* picture);
    void removeOldPictures(bool removeAll = false); // Removes pictures (and including) older than currentPts
    
private:
    
    int srcWidth;
    int srcHeight;
    int dstWidth;
    int dstHeight;
    double frameRate;
    
    int currentFrame;
    
    double currentFrameTime;
    double currentTime;
    double playStartTime;
    
    std::string videoFileName;
    std::function<void(CCVideoSprite*)> videoEndCallback;
    
    AVFormatContext *formatContext;
    AVCodecContext *codecContext;
    SwsContext *swsContext;
    
    int videoStreamIdx;
    int totalLoops;
    
    double currentPts;
    std::map<double, AVPicture*> decodedPictures;
    
    pthread_t decoderThread;
    pthread_mutex_t decoderMutex;
    bool terminateDecoderThread;
    bool decoderThreadRunning;
    
    void startDecodingThread();
    void stopDecodingThread();
    int decodeVideo();
    void displayVideoPicture();
    
    
    
    static void decoderFunc(void*);
    
    static void restartVideoIndef(CCVideoSprite*);
    
};

NS_CC_END


#endif // __CCVIDEOSPRITE_H__