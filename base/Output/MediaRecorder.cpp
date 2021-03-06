//
// Created by 刘轩 on 2019/1/10.
//

#include <EglCore.h>
#include "MediaRecorder.h"

static const uint8_t MemoryAligned = 64; //默认64字节为一个读取单位进行内存对齐

std::string MediaRecordVertexShader =
                "attribute vec4 aPosition;\n"
                "attribute vec4 aTexCoord;\n"
                "varying vec2 vTexCoord;\n"
                "void main() {\n"
                "   gl_Position = aPosition;\n"
                "   vTexCoord = aTexCoord.xy;\n"
                "}\n";

std::string MediaRecordFragmentShader =
                "precision mediump float;\n"
                "varying vec2 vTexCoord;\n"
                "uniform sampler2D uTexture;\n"
                "void main() {\n"
                "    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
                "}\n";

GCVBase::MediaRecorder::MediaRecorder(const GCVBase::EncoderConfig &config, JNIEnv *env) {

    mVideoSize = config.outputSize;
    mediaEncoder = new MediaEncoder(config);
    mediaBuffer =  new MediaBuffer<GLubyte *>();

    auto size = (unsigned long) (mVideoSize.width * mVideoSize.height * 4);
    auto alignSize = alignment_up((size), MemoryAligned);
    mRGBADataSize = alignSize;

    EglCore * shareEglInstance = Context::getShareContext()->getEglInstance();
    EglCore * recordEglInstance = new EglCore(shareEglInstance->getEGLContext(), nullptr, 1, 1);

    /*
     * 记录需要另起一个线程，在mainLooper中进行Camera预览界面的渲染绘制，在recordLooper中进行离屏渲染并保存，
     * 这样可以有效避免预览线程卡顿。同时由于recordLooper所在的线程需要进行离屏渲染，因此也要用到Egl对象，所以这里用到了
     * ShareContext，与mainLooper所在的线程共享egl上下文，可以同时操作同一份纹理（当然此时要做好线程同步工作）
     */
    recordContext = new Context(env, "recordLooper");
    recordContext->setEglInstance(recordEglInstance);

    runSyncContextLooper(recordContext->getContextLooper(), [=]{
        recordContext->useAsCurrentContext();

        mRecordProgram = new GLProgram(MediaRecordVertexShader, MediaRecordFragmentShader);
        if(!mRecordProgram->isProgramLinked()){

            if(!mRecordProgram->linkProgram()){
                // TODO 获取program连接日志`
            }
        }

        aPositionAttribute = mRecordProgram->getAttributeIndex("aPosition");
        aTexCoordAttribute = mRecordProgram->getAttributeIndex("aTexCoord");
        uTextureuniform = mRecordProgram->getuniformIndex("uTexture");
    });
}

GCVBase::MediaRecorder::~MediaRecorder() {
    delete mediaEncoder;
    delete mediaBuffer;
    delete mRecordProgram;
    delete recordContext;
}

void GCVBase::MediaRecorder::startRecording(const std::function<void ()> &handler) {
    mIsRecording = true;
    mStartCallback = handler;

    runSyncContextLooper(recordContext->getContextLooper(), [=]{

        mediaEncoder->startEncoder(mStartCallback);
        mStartCallback = nullptr;
    });
}

void GCVBase::MediaRecorder::pauseRecording(const std::function<void ()> &handler) {
    mIsRecording = false;
    mPauseCallback = handler;

    runSyncContextLooper(recordContext->getContextLooper(), [=]{
        mediaEncoder->pauseEncoder(mPauseCallback);
        mPauseCallback = nullptr;
    });
}

void GCVBase::MediaRecorder::finishRecording(const std::function<void ()> &handler) {
    mIsRecording = false;
    mFinishCallback = handler;

    /**
     * finish的时候需要用异步添加的方式，因为同步的话recordLooper中积压的消息会卡预览线程
     */
    runAsyncContextLooper(recordContext->getContextLooper(), [=]{

        if (!mEncoderIsFinished) {
            mEncoderIsFinished = true;
            mediaEncoder->finishEncoder(mFinishCallback);
            mFinishCallback = nullptr;
        }
    });
}

void GCVBase::MediaRecorder::cancelRecording(const std::function<void ()> &handler) {
    mIsRecording = false;
    mCancelRecording = true;
    mCancelCallback = handler;

    runSyncContextLooper(recordContext->getContextLooper(), [=]{
        mediaEncoder->cancelEncoder(mCancelCallback);
        mCancelCallback = nullptr;
    });
}

void GCVBase::MediaRecorder::_setOutputFramebuffer(GCVBase::FrameBuffer *framebuffer) {
    mFinalFilterFramebuffer = framebuffer;
}

void GCVBase::MediaRecorder::_newFrameReadyAtTime() {
    if(!mIsRecording){
        return;
    }


    glFlush();

    runAsyncContextLooper(recordContext->getContextLooper(), [=]{

        /**
         * 由于异步线程执行的record耗时任务，导致的recordLooper中Message堆积，我们点击了停止按钮之后
         * recordLooper仍然在处理未执行完的 Fuction 函数，也就是本段所在的Lamada表达式。
         *
         * 这本来无可厚非，我们应该等待这些 Fuction 函数执行完毕，但在本段中，我们执行的操作是：
         * 将 mFinalFilterFramebuffer（由Camera传给FilterGroup，再传给MediaRecorder和DisplayView）帧缓冲
         * 渲染到 Recoder 所有的帧缓冲，然后通过 glReadPixels 读取像素保存，这个读取像素的过程是比较耗时的
         *
         * 同时虽然Camera合成的每一帧都会调用到当前方法，也就是每一帧都会产生一个Fuction函数，用于读取
         * mFinalFilterFramebuffer中的像素，但这个 Camera产生的唯一的FBO只能代表当前Camera实时合成的画面，
         * 这意味着并不是每一个Fuction函数能够取得它应取得的像素，也意味着并不是每一帧Camera的像素都能够保证被写入文件，
         *
         * 如此，当我们在击了停止按钮之后，recordLooper仍在处理堆积的Fuction函数，但此时这些Fuction函数中，
         * glReadPixels获取到的画面，是Camera当前实时合成的帧！！！也就是此时仍然在获取并写入点击停止之后的帧！！！
         *
         * 因此我们在这里加了这段 return 判断，用来在点击停止之后丢弃这些还在队列中积压的Fuction函数，确保点击停止
         * 按钮之后确实停止录制。但这样，在整个录制的过程中我们就不可避免的产生了一些丢帧。
         */

        // TODO 解决上述丢帧问题得主要思路就是 fbo缓存，在写入的同时，把Camera实时产生的帧缓冲对象缓存起来，生成一个队列，
        // TODO recordLooper作为消费者去这个队列中去捞缓存起来的 fbo对象，尽可能使Camera产生的每一帧都写入到文件中去
        if(!mIsRecording){
            return;
        }

        renderRecorderFramebuffer(mFinalFilterFramebuffer);

        /**
         * glReadPixels(GLint x,GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid*pixels)
         *
         * glReadPixels 是将GPU渲染完数据，从GPU回传到host端的唯一方式，由入参（x,y,w,h）指定一个从一帧图像中读取内存的矩形，
         * type和format指定格式，pixels是输出像素的buffer
         *
         * 注意：glReadPixels实际上是从缓冲区中读取数据，如果使用了双缓冲区，则默认是从正在显示的缓冲（即前缓冲）中读取，
         * 而绘制工作是默认绘制到后缓冲区的。因此，如果需要读取已经绘制好的像素，往往需要先交换前后缓冲
         */
        auto * mRGBAData = (GLubyte *)(malloc(mRGBADataSize));
        memset(mRGBAData, 0, mRGBADataSize);

        // TODO 同时glReadPixels这个函数其实是效率相对比较低下的，可以考虑用openl es 3.0中的 GL_PIXEL_PACK_BUFFER 代替
        glReadPixels(0,0, (int)mVideoSize.width, (int)mVideoSize.height, GL_RGBA, GL_UNSIGNED_BYTE, mRGBAData);

        mediaBuffer->mediaType = MediaType::Video;
        mediaBuffer->mediaData = mRGBAData;
        mediaBuffer->metaData[WidthKey] = (const void *)(long)mVideoSize.width;
        mediaBuffer->metaData[HeightKey] = (const void *)(long)mVideoSize.height;

        mediaEncoder->newFrameReadyAtTime(mediaBuffer);

    });
}
/**
 * 这里是将外部的FrameBuffer中的texture上的内容渲染到 recordContext 中的 mRecorderFramebuffer上了
 * @param framebuffer 外部的 FrameBuffer
 */
void GCVBase::MediaRecorder::renderRecorderFramebuffer(GCVBase::FrameBuffer *framebuffer) {

    recordContext->useAsCurrentContext();
    mRecordProgram->useProgram();

    creatRecorderFramebuffer();

    glEnableVertexAttribArray(aPositionAttribute);
    glEnableVertexAttribArray(aTexCoordAttribute);

    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, framebuffer->texture());

    static const GLfloat vertices[] = {
            -1.0f, -1.0f,
            1.0f, -1.0f,
            -1.0f,  1.0f,
            1.0f,  1.0f,
    };

    const GLfloat * texCoord = CalculateRecoderRotation(rotationMediaRecorder);

    glUniform1i(uTextureuniform, 4);
    glVertexAttribPointer(aPositionAttribute, 2, GL_FLOAT, 0, 0, vertices);
    glVertexAttribPointer(aTexCoordAttribute, 2, GL_FLOAT, 0, 0, texCoord);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(aPositionAttribute);
    glDisableVertexAttribArray(aTexCoordAttribute);
}

void GCVBase::MediaRecorder::creatRecorderFramebuffer() {
    if (mRecorderFramebuffer) {
        return;
    }

    glActiveTexture(GL_TEXTURE3);
    glGenFramebuffers(1, &mRecorderFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, mRecorderFramebuffer);

    glGenTextures(1, &mRecorderTexture);
    glBindTexture(GL_TEXTURE_2D, mRecorderTexture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (int)mVideoSize.width, (int)mVideoSize.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mRecorderTexture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status != GL_FRAMEBUFFER_COMPLETE){
        // TODO 打log，报错
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mRecorderFramebuffer);
    glViewport(0, 0, (int)mVideoSize.width, (int)mVideoSize.height);
}

void GCVBase::MediaRecorder::bindRecorderFramebuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, mRecorderFramebuffer);
    glViewport(0, 0, (int)mVideoSize.width, (int)mVideoSize.height);
}

void GCVBase::MediaRecorder::unBindRecorderFramebuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GCVBase::MediaRecorder::_setOutputRotation(const GCVBase::Rotation &rotation) {
    rotationMediaRecorder = rotation;
}
