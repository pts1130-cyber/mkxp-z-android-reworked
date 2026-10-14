/*
 ** graphics.cpp
 **
 ** This file is part of mkxp.
 **
 ** Copyright (C) 2013 Jonas Kulla <Nyocurio@gmail.com>
 **
 ** mkxp is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 2 of the License, or
 ** (at your option) any later version.
 **
 ** mkxp is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "graphics.h"

#include "alstream.h"
#include "audio.h"
#include "binding.h"
#include "bitmap.h"
#include "config.h"
#include "debugwriter.h"
#include "disposable.h"
#include "etc-internal.h"
#include "eventthread.h"
#include "filesystem.h"
#include "gl-fun.h"
#include "gl-util.h"
#include "glstate.h"
#include "intrulist.h"
#include "quad.h"
#include "scene.h"
#include "shader.h"
#include "sharedstate.h"
#include "texpool.h"
#include "theoraplay/theoraplay.h"
#include "util.h"
#include "input.h"
#include "sprite.h"
#include "arc-prof.h"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_timer.h>
#include <SDL_video.h>
#include <SDL_mutex.h>
#include <SDL_thread.h>

#ifdef MKXPZ_STEAM
#include "steamshim_child.h"
#endif

#include <algorithm>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <cmath>


#define DEF_SCREEN_W (rgssVer == 1 ? 640 : 544)
#define DEF_SCREEN_H (rgssVer == 1 ? 480 : 416)

#define DEF_FRAMERATE (rgssVer == 1 ? 40 : 60)

#define DEF_MAX_VIDEO_FRAMES 30
#define VIDEO_DELAY 10
#define MOVIE_AUDIO_BUFFER_SIZE 2048
#define AUDIO_BUFFER_LEN_MS 2000

typedef struct AudioQueue
{
    const THEORAPLAY_AudioPacket *audio;
    int offset;
    struct AudioQueue *next;
} AudioQueue;


static long readMovie(THEORAPLAY_Io *io, void *buf, long buflen)
{
    SDL_RWops *f = (SDL_RWops *) io->userdata;
    return (long) SDL_RWread(f, buf, 1, buflen);
} // IoFopenRead


static void closeMovie(THEORAPLAY_Io *io)
{
    SDL_RWops *f = (SDL_RWops *) io->userdata;
    SDL_RWclose(f);
    free(io);
} // IoFopenClose


struct Movie
{
    THEORAPLAY_Decoder *decoder;
    const THEORAPLAY_AudioPacket *audio;
    const THEORAPLAY_VideoFrame *video;
    bool hasVideo;
    bool hasAudio;
    bool skippable;
    Bitmap *videoBitmap;
    SDL_RWops srcOps;
    SDL_Thread *audioThread;
    AtomicFlag audioThreadTermReq;
    volatile AudioQueue *audioQueueHead;
    volatile AudioQueue *audioQueueTail;
    ALuint audioSource;
    ALuint alBuffers[STREAM_BUFS];
    ALshort audioBuffer[MOVIE_AUDIO_BUFFER_SIZE];
    SDL_mutex *audioMutex;
    
    Movie(bool skippable_)
    : decoder(0), audio(0), video(0), skippable(skippable_), videoBitmap(0), audioThread(0)
    {
    }
    bool preparePlayback()
    {
        
        // https://theora.org/doc/libtheora-1.0/codec_8h.html
        // https://ffmpeg.org/doxygen/0.11/group__lavc__misc__pixfmt.html
        THEORAPLAY_Io *io = (THEORAPLAY_Io *) malloc(sizeof (THEORAPLAY_Io));
        if(!io) {
            SDL_RWclose(&srcOps);
            return false;
        }
        
        io->read = readMovie;
        io->close = closeMovie;
        io->userdata = &srcOps;
        decoder = THEORAPLAY_startDecode(io, DEF_MAX_VIDEO_FRAMES, THEORAPLAY_VIDFMT_RGBA);
        if (!decoder) {
            SDL_RWclose(&srcOps);
            return false;
        }
        
        // Wait until the decoder has parsed out some basic truths from the file.
        while (!THEORAPLAY_isInitialized(decoder)) {
            SDL_Delay(VIDEO_DELAY);
        }
        
        // Once we're initialized, we can tell if this file has audio and/or video.
        hasAudio = THEORAPLAY_hasAudioStream(decoder);
        hasVideo = THEORAPLAY_hasVideoStream(decoder);
        
        // Queue up the audio
        if (hasAudio) {
            while ((audio = THEORAPLAY_getAudio(decoder)) == NULL) {
                if ((THEORAPLAY_availableVideo(decoder) >= DEF_MAX_VIDEO_FRAMES)) {
                    break;  // we'll never progress, there's no audio yet but we've prebuffered as much as we plan to.
                }
                SDL_Delay(VIDEO_DELAY);
            }
        }
        
        // No video, so no point in doing anything else
        if (!hasVideo) {
            THEORAPLAY_stopDecode(decoder);
            return false;
        }
        
        // Wait until we have video
        while ((video = THEORAPLAY_getVideo(decoder)) == NULL) {
            SDL_Delay(VIDEO_DELAY);
        }
        
        // Wait until we have audio, if applicable
        audio = NULL;
        if (hasAudio) {
            while ((audio = THEORAPLAY_getAudio(decoder)) == NULL && THEORAPLAY_availableVideo(decoder) < DEF_MAX_VIDEO_FRAMES) {
                SDL_Delay(VIDEO_DELAY);
            }
        }
        videoBitmap = new Bitmap(video->width, video->height);
        audioQueueHead = NULL;
        audioQueueTail = NULL;
        
        return true;
    }
    
    void queueAudioPacket(const THEORAPLAY_AudioPacket *audio) {
        AudioQueue *item = NULL;
        
        if (!audio) {
            return;
        }
        
        item = (AudioQueue *) malloc(sizeof (AudioQueue));
        if (!item) {
            THEORAPLAY_freeAudio(audio);
            return;  // oh well.
        }
        
        item->audio = audio;
        item->offset = 0;
        item->next = NULL;
        
        SDL_LockMutex(audioMutex);
        if (audioQueueTail) {
            audioQueueTail->next = item;
        } else {
            audioQueueHead = item;
        }
        audioQueueTail = item;
        SDL_UnlockMutex(audioMutex);
    }
    
    void bufferMovieAudio(THEORAPLAY_Decoder *decoder, const Uint32 now) {
        const THEORAPLAY_AudioPacket *audio;
        while ((audio = THEORAPLAY_getAudio(decoder)) != NULL) {
            queueAudioPacket(audio);
            if (audio->playms >= now + AUDIO_BUFFER_LEN_MS) {  // don't let this get too far ahead.
                break;
            }
        }
    }

    void streamMovieAudio(){
        ALint state = 0;
        ALint procBufs = STREAM_BUFS;	    
        volatile AudioQueue *audioPacketAndOffset;
        int channels;
        int sampleRate;
        float *sourceSamples;
        ALuint samplesToProcess;
        ALshort *sampleBuffer;
        ALuint remainingSamples;

        while(true) {
            while(procBufs--) {
                // Quit if audio thread terminate request has been made
                if (audioThreadTermReq) return;

                remainingSamples = MOVIE_AUDIO_BUFFER_SIZE;
                sampleBuffer = audioBuffer;
                SDL_LockMutex(audioMutex);

                while(audioQueueHead && (remainingSamples > 0)) {
                    audioPacketAndOffset = audioQueueHead;
                    channels = audioPacketAndOffset->audio->channels;
                    sampleRate = audioPacketAndOffset->audio->freq;
                    sourceSamples = audioPacketAndOffset->audio->samples + (audioPacketAndOffset->offset * channels);
                    samplesToProcess = (audioPacketAndOffset->audio->frames - audioPacketAndOffset->offset) * channels;

                    if (samplesToProcess > remainingSamples) samplesToProcess = remainingSamples;

                    for (ALuint i = 0; i < samplesToProcess; i++) {
                        const float val = (*(sourceSamples++));
                        if (val < -1.0f) {
                            *(sampleBuffer++) = SHRT_MIN;
                        } else if (val > 1.0f) {
                            *(sampleBuffer++) = SHRT_MAX;
                        } else {
                            *(sampleBuffer++) = (ALshort) (val * SHRT_MAX);
                        }
                    }

                    // Necessary to remember position between repeated iterations
                    audioPacketAndOffset->offset += (samplesToProcess / channels);
                    remainingSamples -= samplesToProcess;

                    // The current audio packet has been completed
                    if ((audioPacketAndOffset->offset) >= audioPacketAndOffset->audio->frames) {
                        audioQueueHead = audioPacketAndOffset->next;
                        THEORAPLAY_freeAudio(audioPacketAndOffset->audio);
                        free((void *) audioPacketAndOffset);
                    }
                }

                if(!audioQueueHead) audioQueueTail = NULL;

                SDL_UnlockMutex(audioMutex);

                alBufferData(alBuffers[procBufs], channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, audioBuffer,
                    (MOVIE_AUDIO_BUFFER_SIZE - remainingSamples) * sizeof(ALshort), sampleRate);
                alSourceQueueBuffers(audioSource, 1, &alBuffers[procBufs]);     
                alGetSourcei(audioSource, AL_SOURCE_STATE, &state);
                if(state != AL_PLAYING) alSourcePlay(audioSource);
            }

            // Periodically check the buffers until one is available
            while(true) {
                alGetSourcei(audioSource, AL_BUFFERS_PROCESSED, &procBufs);
                if(procBufs > 0) break;
                SDL_Delay(AUDIO_SLEEP);
            }
            alSourceUnqueueBuffers(audioSource, procBufs, alBuffers);
        }
    }
    
    bool startAudio(float volume)
    {
        alGenSources(1, &audioSource);
        alGenBuffers(STREAM_BUFS, alBuffers);
        alSourcef(audioSource, AL_GAIN, volume);

        audioThreadTermReq.clear();
        audioMutex = SDL_CreateMutex();
        queueAudioPacket(audio);
        audio = NULL;
        bufferMovieAudio(decoder, 0);
        audioThread = createSDLThread <Movie, &Movie::streamMovieAudio>(this, "movieaudio");

        return true;
    }
    
    void play(float volume)
    {
        Uint32 frameMs = 0;
        Uint32 baseTicks = SDL_GetTicks();
        bool openedAudio = false;
        while (THEORAPLAY_isDecoding(decoder)) {
            // Check for reset/shutdown input
            if(shState->graphics().updateMovieInput(this)) break;
            
            // Check for attempted skip
            if (skippable) {
                shState->input().update();
                if  (shState->input().isTriggered(Input::C) || shState->input().isTriggered(Input::B)) break;
            }
            
            const Uint32 now = SDL_GetTicks() - baseTicks;
            
            if (!video) {
                video = THEORAPLAY_getVideo(decoder);
            }
            
            if (hasAudio) {
                if (!audio) {
                    audio = THEORAPLAY_getAudio(decoder);
                }
                
                if (audio && !openedAudio) {
                    if(!startAudio(volume)){
                        Debug() << "Error opening movie audio!";
                        break;
                    }
                    openedAudio = true;
                }
                
            }
            
            if (video && (video->playms <= now)) {
                frameMs = (video->fps == 0.0) ? 0 : ((Uint32) (1000.0 / video->fps));
                if ( frameMs && ((now - video->playms) >= frameMs) )
                {
                    // Skip frames to catch up
                    const THEORAPLAY_VideoFrame *last = video;
                    while ((video = THEORAPLAY_getVideo(decoder)) != NULL)
                    {
                        THEORAPLAY_freeVideo(last);
                        last = video;
                        if ((now - video->playms) < frameMs)
                            break;
                    } 

                    if (!video)
                        video = last;
                }

                // Application is too far behind
                if (!video) {
                    Debug() << "WARNING: Video playback cannot keep up!";
                    break;
                }

                // Got a video frame, now draw it
                videoBitmap->replaceRaw(video->pixels, video->width * video->height * 4);
                shState->graphics().update(false);
                THEORAPLAY_freeVideo(video);
                video = NULL;

            } else {
                // Next video frame not yet ready, let the CPU breathe
                SDL_Delay(VIDEO_DELAY);
            }
            
            if (openedAudio) {
                bufferMovieAudio(decoder, now);
            }
        }
    }
    
    ~Movie()
    {
        if (hasAudio) {
            if (audioQueueTail) {
                THEORAPLAY_freeAudio(audioQueueTail->audio);
            }
            audioQueueTail = NULL;
            
            if (audioQueueHead) {
                THEORAPLAY_freeAudio(audioQueueHead->audio);
            }
            audioQueueHead = NULL;
        }
        SDL_DestroyMutex(audioMutex);
        audioThreadTermReq.set();
        if(audioThread) {
            SDL_WaitThread(audioThread, 0);
            audioThread = 0;
        }
        alSourceStop(audioSource);
        alDeleteSources(1, &audioSource);
        alDeleteBuffers(STREAM_BUFS, alBuffers);
        if (video) THEORAPLAY_freeVideo(video);
        if (audio) THEORAPLAY_freeAudio(audio);
        if (decoder) THEORAPLAY_stopDecode(decoder);
        delete videoBitmap;
    }
};

struct MovieOpenHandler : FileSystem::OpenHandler
{
    SDL_RWops *srcOps;
    
    MovieOpenHandler(SDL_RWops &srcOps)
    :   srcOps(&srcOps)
    {}
    
    bool tryRead(SDL_RWops &ops, const char *ext)
    {
        *srcOps = ops;
        return true;
    }
};

struct PingPong {
    TEXFBO rt[2];
    uint8_t srcInd, dstInd;
    int screenW, screenH;
    
    PingPong(int screenW, int screenH)
    : srcInd(0), dstInd(1), screenW(screenW), screenH(screenH) {
        for (int i = 0; i < 2; ++i) {
            TEXFBO::init(rt[i]);
            TEXFBO::allocEmpty(rt[i], screenW, screenH);
            TEXFBO::linkFBO(rt[i]);
            gl.ClearColor(0, 0, 0, 1);
            FBO::clear();
        }
    }
    
    ~PingPong() {
        for (int i = 0; i < 2; ++i)
            TEXFBO::fini(rt[i]);
    }
    
    TEXFBO &backBuffer() { return rt[srcInd]; }
    
    TEXFBO &frontBuffer() { return rt[dstInd]; }
    
    /* Better not call this during render cycles */
    void resize(int width, int height) {
        screenW = width;
        screenH = height;
        
        for (int i = 0; i < 2; ++i)
            TEXFBO::allocEmpty(rt[i], width, height);
    }
    
    void startRender() { bind(); }
    
    void swapRender() {
        std::swap(srcInd, dstInd);
        
        bind();
    }
    
    void clearBuffers() {
        glState.clearColor.pushSet(Vec4(0, 0, 0, 1));
        
        for (int i = 0; i < 2; ++i) {
            FBO::bind(rt[i].fbo);
            FBO::clear();
        }
        
        glState.clearColor.pop();
    }
    
private:
    void bind() { FBO::bind(rt[dstInd].fbo); }
};

class ScreenScene : public Scene {
public:
    ScreenScene(int width, int height) : pp(width, height) {
        updateReso(width, height);
        
        brightEffect = false;
        brightnessQuad.setColor(Vec4());
    }
    
    void composite() {
        const int w = geometry.rect.w;
        const int h = geometry.rect.h;
        
        shState->prepareDraw();
        
        pp.startRender();
        
        glState.viewport.set(IntRect(0, 0, w, h));
        
        FBO::clear();
        
        Scene::composite();
        
        if (brightEffect) {
            SimpleColorShader &shader = shState->shaders().simpleColor;
            shader.bind();
            shader.applyViewportProj();
            shader.setTranslation(Vec2i());
            
            brightnessQuad.draw();
        }
    }
    
    void requestViewportRender(const Vec4 &c, const Vec4 &f, const Vec4 &t) {
        const IntRect &viewpRect = glState.scissorBox.get();
        const IntRect &screenRect = geometry.rect;
        
        const bool toneRGBEffect = t.xyzNotNull();
        const bool toneGrayEffect = t.w != 0;
        const bool colorEffect = c.w > 0;
        const bool flashEffect = f.w > 0;
        
        if (toneGrayEffect) {
            pp.swapRender();
            
            if (!viewpRect.encloses(screenRect)) {
                /* Scissor test _does_ affect FBO blit operations,
                 * and since we're inside the draw cycle, it will
                 * be turned on, so turn it off temporarily */
                glState.scissorTest.pushSet(false);
                
                GLMeta::blitBegin(pp.frontBuffer());
                GLMeta::blitSource(pp.backBuffer());
                GLMeta::blitRectangle(geometry.rect, Vec2i());
                GLMeta::blitEnd();
                
                glState.scissorTest.pop();
            }
            
            GrayShader &shader = shState->shaders().gray;
            shader.bind();
            shader.setGray(t.w);
            shader.applyViewportProj();
            shader.setTexSize(screenRect.size());
            
            TEX::bind(pp.backBuffer().tex);
            
            glState.blend.pushSet(false);
            screenQuad.draw();
            glState.blend.pop();
        }
        
        if (!toneRGBEffect && !colorEffect && !flashEffect)
            return;
        
        FlatColorShader &shader = shState->shaders().flatColor;
        shader.bind();
        shader.applyViewportProj();
        
        if (toneRGBEffect) {
            /* First split up additive / substractive components */
            Vec4 add, sub;
            
            if (t.x > 0)
                add.x = t.x;
            if (t.y > 0)
                add.y = t.y;
            if (t.z > 0)
                add.z = t.z;
            
            if (t.x < 0)
                sub.x = -t.x;
            if (t.y < 0)
                sub.y = -t.y;
            if (t.z < 0)
                sub.z = -t.z;
            
            /* Then apply them using hardware blending */
            gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            
            if (add.xyzNotNull()) {
                gl.BlendEquation(GL_FUNC_ADD);
                shader.setColor(add);
                
                screenQuad.draw();
            }
            
            if (sub.xyzNotNull()) {
                gl.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                shader.setColor(sub);
                
                screenQuad.draw();
            }
        }
        
        if (colorEffect || flashEffect) {
            gl.BlendEquation(GL_FUNC_ADD);
            gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                 GL_ONE);
        }
        
        if (colorEffect) {
            shader.setColor(c);
            screenQuad.draw();
        }
        
        if (flashEffect) {
            shader.setColor(f);
            screenQuad.draw();
        }
        
        glState.blendMode.refresh();
    }
    
    void setBrightness(float norm) {
        brightnessQuad.setColor(Vec4(0, 0, 0, 1.0f - norm));
        
        brightEffect = norm < 1.0f;
    }
    
    void updateReso(int width, int height) {
        geometry.rect.w = width;
        geometry.rect.h = height;
        
        screenQuad.setTexPosRect(geometry.rect, geometry.rect);
        brightnessQuad.setTexPosRect(geometry.rect, geometry.rect);
        
        notifyGeometryChange();
    }
    
    void setResolution(int width, int height) {
        pp.resize(width, height);
        updateReso(width, height);
    }
    
    PingPong &getPP() { return pp; }
    
private:
    PingPong pp;
    Quad screenQuad;
    
    Quad brightnessQuad;
    bool brightEffect;
};

/* Nanoseconds per second */
#define NS_PER_S 1000000000

struct FPSLimiter {
    uint64_t lastTickCount;
    
    /* ticks per frame */
    int64_t tpf;
    
    /* Ticks per second */
    const uint64_t tickFreq;
    
    /* Ticks per milisecond */
    const uint64_t tickFreqMS;
    
    /* Ticks per nanosecond */
    const double tickFreqNS;
    
    bool disabled;
    
    /* Data for frame timing adjustment */
    struct {
        /* Last tick count */
        uint64_t last;
        
        /* How far behind/in front we are for ideal frame timing */
        int64_t idealDiff;
        
        bool resetFlag;
    } adj;
    
    FPSLimiter(uint16_t desiredFPS)
    : lastTickCount(SDL_GetPerformanceCounter()),
    tickFreq(SDL_GetPerformanceFrequency()), tickFreqMS(tickFreq / 1000),
    tickFreqNS((double)tickFreq / NS_PER_S), disabled(false) {
        setDesiredFPS(desiredFPS);
        
        adj.last = SDL_GetPerformanceCounter();
        adj.idealDiff = 0;
        adj.resetFlag = false;
    }
    
    void setDesiredFPS(uint16_t value) { tpf = tickFreq / value; }
    
    void delay() {
        if (disabled)
            return;
        
        int64_t tickDelta = SDL_GetPerformanceCounter() - lastTickCount;
        int64_t toDelay = tpf - tickDelta;
        
        /* Compensate for the last delta
         * to the ideal timestep */
        toDelay -= adj.idealDiff;
        
        if (toDelay < 0)
            toDelay = 0;
        
        delayTicks(toDelay);
        
        uint64_t now = lastTickCount = SDL_GetPerformanceCounter();
        int64_t diff = now - adj.last;
        adj.last = now;
        
        /* Recalculate our temporal position
         * relative to the ideal timestep */
        adj.idealDiff = diff - tpf + adj.idealDiff;
        
        if (adj.resetFlag) {
            adj.idealDiff = 0;
            adj.resetFlag = false;
        }
    }
    
    void resetFrameAdjust() { adj.resetFlag = true; }
    
    /* If we're more than a full frame's worth
     * of ticks behind the ideal timestep,
     * there's no choice but to skip frame(s)
     * to catch up */
    bool frameSkipRequired() const {
        if (disabled)
            return false;
        
        return adj.idealDiff > tpf;
    }
    
private:
    void delayTicks(uint64_t ticks) {
#if defined(HAVE_NANOSLEEP)
        struct timespec req;
        uint64_t nsec = ticks / tickFreqNS;
        req.tv_sec = nsec / NS_PER_S;
        req.tv_nsec = nsec % NS_PER_S;
        errno = 0;
        
        while (nanosleep(&req, &req) == -1) {
            int err = errno;
            errno = 0;
            
            if (err == EINTR)
                continue;
            
            Debug() << "nanosleep failed. errno:" << err;
            SDL_Delay(ticks / tickFreqMS);
            break;
        }
#else
        SDL_Delay(ticks / tickFreqMS);
#endif
    }
};


/* ===========================================================================
 * ARC: frame profiler.  LOG ONLY - writes _log/engine_gfx.txt and changes
 * nothing about what is drawn or how fast the game runs.
 *
 * Every buffer swap closes one "frame record" (time since the previous swap):
 *   루비  = everything outside the engine's own drawing (game scripts)
 *   합성  = screen.composite(): all Sprites/Windows/... issue their GL calls
 *   그림  = final scale-up blit to the window
 *   대기  = frame limiter sleep (waiting for the next 1/40 s slot)
 *   교체  = SDL_GL_SwapWindow (blocks when the GPU / display is behind)
 *   GPU   = real GPU time of composite+blit (EXT_disjoint_timer_query), if the
 *           phone's driver offers it
 * ======================================================================== */
#define ARC_GFX_LOG         "_log/engine_gfx.txt"
#define ARC_SLOW_MS         40.0
#define ARC_SUMMARY_EVERY   300
#define ARC_SLOW_LINES_MAX  250
#define ARC_SUM_LINES_MAX   240
#define ARC_EVT_LINES_MAX   120

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#define ARC_GL_QUERY_RESULT            0x8866
#define ARC_GL_QUERY_RESULT_AVAILABLE  0x8867
#define ARC_GL_GPU_DISJOINT            0x8FBB

typedef void (APIENTRYP ArcPFNGenQueries)(GLsizei n, GLuint *ids);
typedef void (APIENTRYP ArcPFNBeginQuery)(GLenum target, GLuint id);
typedef void (APIENTRYP ArcPFNEndQuery)(GLenum target);
typedef void (APIENTRYP ArcPFNGetQueryObjuiv)(GLuint id, GLenum pname, GLuint *params);
typedef void (APIENTRYP ArcPFNGetQueryObjui64v)(GLuint id, GLenum pname, uint64_t *params);

struct ArcGpuTimer
{
    enum { N = 6 };

    bool tried;
    bool ok;
    bool active;
    GLuint ids[N];
    int tail;
    int count;

    ArcPFNGenQueries genQ;
    ArcPFNBeginQuery beginQ;
    ArcPFNEndQuery endQ;
    ArcPFNGetQueryObjuiv getU;
    ArcPFNGetQueryObjui64v getU64;

    double lastMs;
    double sumMs;
    double maxMs;
    uint32_t n;

    void init()
    {
        tried = true;

        /* escape hatch: an (empty) file _log/gpu_timer.off turns the GPU timer off */
        if (access("_log/gpu_timer.off", F_OK) == 0)
            return;

        const char *ext = (const char *) gl.GetString(GL_EXTENSIONS);

        if (!ext || !strstr(ext, "GL_EXT_disjoint_timer_query"))
            return;

        genQ = (ArcPFNGenQueries) SDL_GL_GetProcAddress("glGenQueriesEXT");
        beginQ = (ArcPFNBeginQuery) SDL_GL_GetProcAddress("glBeginQueryEXT");
        endQ = (ArcPFNEndQuery) SDL_GL_GetProcAddress("glEndQueryEXT");
        getU = (ArcPFNGetQueryObjuiv) SDL_GL_GetProcAddress("glGetQueryObjectuivEXT");
        getU64 = (ArcPFNGetQueryObjui64v) SDL_GL_GetProcAddress("glGetQueryObjectui64vEXT");

        if (!genQ || !beginQ || !endQ || !getU || !getU64)
            return;

        genQ(N, ids);

        /* reading the disjoint flag once clears it */
        GLint dj = 0;
        gl.GetIntegerv(ARC_GL_GPU_DISJOINT, &dj);

        ok = true;
    }

    void begin()
    {
        if (!tried)
            init();

        if (!ok)
            return;

        if (active)
            end();

        if (count >= N)
            return;

        beginQ(GL_TIME_ELAPSED_EXT, ids[(tail + count) % N]);
        active = true;
    }

    void end()
    {
        if (!ok || !active)
            return;

        endQ(GL_TIME_ELAPSED_EXT);
        ++count;
        active = false;
    }

    /* collect finished measurements, oldest first, never waits */
    void poll()
    {
        if (!ok)
            return;

        while (count > 0)
        {
            GLuint avail = 0;
            getU(ids[tail], ARC_GL_QUERY_RESULT_AVAILABLE, &avail);

            if (!avail)
                break;

            uint64_t ns = 0;
            getU64(ids[tail], ARC_GL_QUERY_RESULT, &ns);

            GLint dj = 0;
            gl.GetIntegerv(ARC_GL_GPU_DISJOINT, &dj);

            tail = (tail + 1) % N;
            --count;

            if (dj)
                continue;

            lastMs = ns / 1000000.0;
            sumMs += lastMs;

            if (lastMs > maxMs)
                maxMs = lastMs;

            ++n;
        }
    }
};

struct ArcGfxProf
{
    bool started;
    bool headerDone;
    bool useFrameSkipNow;

    uint64_t tPrevEnd;   /* end of the previous swap */
    uint64_t tDraw;      /* start of composite */
    uint64_t tComp;      /* end of composite (0 = no composite this frame) */

    double msComp, msBlit, msDelay, msSwap, msSkipDelay;
    uint32_t skipped;
    uint64_t seq;

    ArcGlCount glPrev;

    ArcElemBin bins[ArcElemProf::MAX_BINS];
    int nbins;

    /* running totals for the summary line */
    uint32_t wN, wS30, wS50, wS80, wSkip;
    double wInt, wMax, wRuby, wComp, wBlit, wDelay, wSwap;
    ArcGlCount glWin;
    uint64_t rwCallsWin, rwBytesWin, imgTicksWin;
    uint32_t imgCountWin;

    ArcGpuTimer gpu;

    int slowLines, sumLines, evtLines;

    void drawBegin()
    {
        tDraw = SDL_GetPerformanceCounter();
        tComp = 0;
        arcElemProf().reset();
        gpu.begin();
    }

    void compositeEnd()
    {
        tComp = SDL_GetPerformanceCounter();
        msComp = arcTicksToMs(tComp - tDraw);

        ArcElemProf &ep = arcElemProf();
        nbins = ep.nbins;

        for (int i = 0; i < nbins; ++i)
            bins[i] = ep.bins[i];
    }

    void skipFrame(double delayMs)
    {
        ++skipped;
        msSkipDelay += delayMs;
    }

    void swapStart(uint64_t t0)
    {
        msBlit = tComp ? arcTicksToMs(t0 - tComp) : 0;
        gpu.end();
    }

    void resetWindow()
    {
        wN = wS30 = wS50 = wS80 = wSkip = 0;
        wInt = wMax = wRuby = wComp = wBlit = wDelay = wSwap = 0;
        glWin = arcGlCount();
        rwCallsWin = arcRwCalls;
        rwBytesWin = arcRwBytes;
        imgTicksWin = arcImgTicks;
        imgCountWin = arcImgCount;
        gpu.sumMs = 0;
        gpu.maxMs = 0;
        gpu.n = 0;
    }

    void logSlow(double interval, double ruby)
    {
        if (slowLines >= ARC_SLOW_LINES_MAX)
            return;

        ++slowLines;

        const ArcGlCount &g = arcGlCount();
        char clk[16];
        char el[320];
        char gpuTxt[32];
        el[0] = '\0';

        if (!gpu.ok)
            snprintf(gpuTxt, sizeof(gpuTxt), "없음");
        else if (gpu.n)
            snprintf(gpuTxt, sizeof(gpuTxt), "%.1fms", gpu.lastMs);
        else
            snprintf(gpuTxt, sizeof(gpuTxt), "아직");

        for (int i = 0; i < nbins; ++i)
        {
            size_t len = strlen(el);

            if (len + 48 >= sizeof(el))
                break;

            snprintf(el + len, sizeof(el) - len, "%s%s %u건 %.1fms",
                     i ? " · " : "", bins[i].name, bins[i].n, bins[i].ms);
        }

        arcLogLine(2, ARC_GFX_LOG, 1000000,
            "[%s 엔진 %.1f초] 느린 프레임 #%llu %.0fms | 루비 %.1f 합성 %.1f 그림 %.1f 대기 %.1f 교체 %.1f 건너뜀 %u"
            " | 그리기 %u FBO %u 프로그램 %u 올림 %u+%u(%.1fMB %.1fms) 읽어오기 %u(%.1fms) GPU(최근) %s"
            " | 요소: %s",
            arcClockStr(clk), SDL_GetTicks() / 1000.0,
            (unsigned long long) seq, interval, ruby, msComp, msBlit, msDelay, msSwap, skipped,
            g.draws - glPrev.draws, g.fboBinds - glPrev.fboBinds,
            g.progSwitches - glPrev.progSwitches,
            g.texImages - glPrev.texImages, g.texSubImages - glPrev.texSubImages,
            (g.texBytes - glPrev.texBytes) / 1048576.0,
            arcTicksToMs(g.texTicks - glPrev.texTicks),
            g.readPixels - glPrev.readPixels,
            arcTicksToMs(g.readTicks - glPrev.readTicks),
            gpuTxt,
            el[0] ? el : "(없음)");
    }

    void logSummary()
    {
        if (sumLines < ARC_SUM_LINES_MAX)
        {
            ++sumLines;

            const ArcGlCount &g = arcGlCount();
            double n = wN ? wN : 1;
            char clk[16];
            char gpuTxt[64];

            if (!gpu.ok)
                snprintf(gpuTxt, sizeof(gpuTxt), "GPU타이머 없음");
            else if (gpu.n)
                snprintf(gpuTxt, sizeof(gpuTxt), "GPU 평균 %.1fms 최대 %.1fms(%u회)",
                         gpu.sumMs / gpu.n, gpu.maxMs, gpu.n);
            else
                snprintf(gpuTxt, sizeof(gpuTxt), "GPU 결과 아직 없음");

            arcLogLine(2, ARC_GFX_LOG, 1000000,
                "[%s 엔진 %.1f초] 요약 최근 %u프레임 | 평균 %.1fms 최대 %.0fms · 30ms넘음 %u · 50ms넘음 %u · 80ms넘음 %u · 건너뜀 %u(frameSkip=%d)"
                " | 루비 %.1f 합성 %.1f 그림 %.1f 대기 %.1f 교체 %.1f"
                " | 프레임당 그리기 %.0f FBO %.0f 프로그램 %.0f 올림 %.1f+%.1f(%.2fMB, %.1fms) 읽어오기 %.2f(%.1fms 합)"
                " | %s | 파일읽기 %llu회 %lluKB · 그림불러오기 %u장 %.0fms",
                arcClockStr(clk), SDL_GetTicks() / 1000.0, wN,
                wInt / n, wMax, wS30, wS50, wS80, wSkip, useFrameSkipNow ? 1 : 0,
                wRuby / n, wComp / n, wBlit / n, wDelay / n, wSwap / n,
                (g.draws - glWin.draws) / n, (g.fboBinds - glWin.fboBinds) / n,
                (g.progSwitches - glWin.progSwitches) / n,
                (g.texImages - glWin.texImages) / n, (g.texSubImages - glWin.texSubImages) / n,
                (g.texBytes - glWin.texBytes) / 1048576.0 / n,
                arcTicksToMs(g.texTicks - glWin.texTicks) / n,
                (g.readPixels - glWin.readPixels) / n,
                arcTicksToMs(g.readTicks - glWin.readTicks),
                gpuTxt,
                (unsigned long long) (arcRwCalls - rwCallsWin),
                (unsigned long long) ((arcRwBytes - rwBytesWin) / 1024),
                arcImgCount - imgCountWin, arcTicksToMs(arcImgTicks - imgTicksWin));
        }

        resetWindow();
    }

    void frameEnd(uint64_t tEnd, double delayMs, double swapMs)
    {
        msDelay = delayMs;
        msSwap = swapMs;
        ++seq;

        if (!started)
        {
            started = true;
            tPrevEnd = tEnd;
            glPrev = arcGlCount();
            resetWindow();
            msComp = msBlit = msSkipDelay = 0;
            skipped = 0;
            tComp = 0;
            return;
        }

        double interval = arcTicksToMs(tEnd - tPrevEnd);
        double ruby = interval - msComp - msBlit - msDelay - msSwap - msSkipDelay;

        if (ruby < 0)
            ruby = 0;

        gpu.poll();

        ++wN;
        wInt += interval;

        if (interval > wMax)
            wMax = interval;

        wRuby += ruby;
        wComp += msComp;
        wBlit += msBlit;
        wDelay += msDelay;
        wSwap += msSwap;
        wSkip += skipped;

        if (interval >= 30.0) ++wS30;
        if (interval >= 50.0) ++wS50;
        if (interval >= 80.0) ++wS80;

        if (interval >= ARC_SLOW_MS)
            logSlow(interval, ruby);

        if (wN >= ARC_SUMMARY_EVERY)
            logSummary();

        tPrevEnd = tEnd;
        glPrev = arcGlCount();
        msComp = msBlit = msSkipDelay = 0;
        skipped = 0;
        tComp = 0;
    }
};

static ArcGfxProf arcGfx;

struct GraphicsPrivate {
    /* Screen resolution, ie. the resolution at which
     * RGSS renders at (settable with Graphics.resize_screen).
     * Can only be changed from within RGSS */
    Vec2i scRes;
    
    /* Screen size, to which the rendered frames are scaled up.
     * This can be smaller than the window size when fixed aspect
     * ratio is enforced */
    Vec2i scSize;
    
    /* Actual physical size of the game window */
    Vec2i winSize;
    
    /* Offset in the game window at which the scaled game screen
     * is blitted inside the game window */
    Vec2i scOffset;
    
    // Scaling factor, used to display the screen properly
    // on Retina displays
    int scalingFactor;
    
    ScreenScene screen;
    RGSSThreadData *threadData;
    SDL_GLContext glCtx;
    
    int frameRate;
    int frameCount;
    int brightness;
    
    unsigned long long last_update;
    
    
    FPSLimiter fpsLimiter;
    
    // Can be set from Ruby. Takes priority over config setting.
    bool useFrameSkip;
    
    bool frozen;
    TEXFBO frozenScene;
    Quad screenQuad;
    
    float backingScaleFactor;
    
    Vec2i integerScaleFactor;
    TEXFBO integerScaleBuffer;
    bool integerScaleActive;
    bool integerLastMileScaling;
    
    std::vector<unsigned long long> avgFPSData;
    unsigned long long last_avg_update;
    SDL_mutex *avgFPSLock;
    
    SDL_mutex *glResourceLock;
    bool multithreadedMode;
    
    /* Global list of all live Disposables
     * (disposed on reset) */
    IntruList<Disposable> dispList;
    
    GraphicsPrivate(RGSSThreadData *rtData)
    : scRes(DEF_SCREEN_W, DEF_SCREEN_H), scSize(scRes),
    winSize(rtData->config.defScreenW, rtData->config.defScreenH),
    screen(scRes.x, scRes.y), threadData(rtData),
    glCtx(SDL_GL_GetCurrentContext()), multithreadedMode(true),
    frameRate(DEF_FRAMERATE), frameCount(0), brightness(255),
    fpsLimiter(frameRate), useFrameSkip(rtData->config.frameSkip), frozen(false),
    last_update(0), last_avg_update(0), backingScaleFactor(1), integerScaleFactor(0, 0),
    integerScaleActive(rtData->config.integerScaling.active),
    integerLastMileScaling(rtData->config.integerScaling.lastMileScaling) {
        avgFPSData = std::vector<unsigned long long>();
        avgFPSLock = SDL_CreateMutex();
        glResourceLock = SDL_CreateMutex();
        
        if (integerScaleActive) {
            integerScaleFactor = Vec2i(0, 0);
            rebuildIntegerScaleBuffer();
        }
        
        recalculateScreenSize(rtData->config.fixedAspectRatio);
        updateScreenResoRatio(rtData);
        
        TEXFBO::init(frozenScene);
        TEXFBO::allocEmpty(frozenScene, scRes.x, scRes.y);
        TEXFBO::linkFBO(frozenScene);
        
        FloatRect screenRect(0, 0, scRes.x, scRes.y);
        screenQuad.setTexPosRect(screenRect, screenRect);
        
        fpsLimiter.resetFrameAdjust();
    }
    
    ~GraphicsPrivate() {
        TEXFBO::fini(frozenScene);
        TEXFBO::fini(integerScaleBuffer);
        SDL_DestroyMutex(avgFPSLock);
        SDL_DestroyMutex(glResourceLock);
    }
    
    void updateScreenResoRatio(RGSSThreadData *rtData) {
        Vec2 &ratio = rtData->sizeResoRatio;
        ratio.x = (float)scRes.x / scSize.x * backingScaleFactor;
        ratio.y = (float)scRes.y / scSize.y * backingScaleFactor;
        
        rtData->screenOffset = scOffset / backingScaleFactor;
        /* 마우스·터치 좌표는 위에서부터이므로 y 는 위쪽 여백으로 (가운데 정렬일 땐 값이 같음) */
        rtData->screenOffset.y = (winSize.y - scOffset.y - scSize.y) / backingScaleFactor;
    }
    
    /* Enforces fixed aspect ratio, if desired */
    void recalculateScreenSize(bool fixedAspectRatio) {
        scSize = winSize;
        
        if (!fixedAspectRatio && integerLastMileScaling) {
            scOffset = Vec2i(0, 0);
            return;
        }
        
        if (integerScaleActive && !integerLastMileScaling) {
            scOffset.x = ((winSize.x / 2) - (scRes.x / 2) * integerScaleFactor.x);
            scOffset.y = ((winSize.y / 2) - (scRes.y / 2) * integerScaleFactor.y);
            
            scSize = Vec2i(scRes.x * integerScaleFactor.x, scRes.y * integerScaleFactor.y);
            return;
        }
        
        float resRatio = (float)scRes.x / scRes.y;
        float winRatio = (float)winSize.x / winSize.y;
        
        if (resRatio > winRatio)
            scSize.y = scSize.x / resRatio;
        else if (resRatio < winRatio)
            scSize.x = scSize.y * resRatio;
        
        scOffset.x = (winSize.x - scSize.x) / 2.f;
        scOffset.y = (winSize.y - scSize.y) / 2.f;
        
        /* ARC: 세로 화면(폰을 세워 쓸 때)에서는 가운데가 아니라 위쪽에 둠 — 조이플레이와 같은 높이(위에서 화면 높이의 24.5%).
         * 크기는 그대로(폭에 맞춤)이고 위치만 올라감. 터치 좌표는 screenOffset 으로 같이 보정됨. 아래 여백이 조작 버튼 자리. */
        if (winSize.y > winSize.x && scSize.y < winSize.y) {
            int top = (int)(winSize.y * 0.245f);          /* 화면 위에서부터의 거리 */
            int maxTop = winSize.y - scSize.y;
            if (top > maxTop) top = maxTop;
            scOffset.y = winSize.y - top - scSize.y;      /* scOffset.y 는 GL 좌표(아래에서부터)라서 변환 */
        }
    }
    
    static int findHighestFittingScale(int base, int target) {
        int scale = 1;
        
        while (base * scale <= target)
            scale++;
        
        return std::max(scale - 1, 1);
    }
    
    /* Returns whether a new scale was found */
    bool findHighestIntegerScale()
    {
        Vec2i newScale(findHighestFittingScale(scRes.x, winSize.x),
                       findHighestFittingScale(scRes.y, winSize.y));
        
        if (threadData->config.fixedAspectRatio)
        {
            /* Limit both factors to the smaller of the two */
            newScale.x = newScale.y = std::min(newScale.x, newScale.y);
        }
        
        if (newScale == integerScaleFactor)
            return false;
        
        integerScaleFactor = newScale;
        return true;
    }
    
    void rebuildIntegerScaleBuffer()
    {
        TEXFBO::fini(integerScaleBuffer);
        TEXFBO::init(integerScaleBuffer);
        TEXFBO::allocEmpty(integerScaleBuffer, scRes.x * integerScaleFactor.x,
                           scRes.y * integerScaleFactor.y);
        TEXFBO::linkFBO(integerScaleBuffer);
    }
    
    bool integerScaleStepApplicable() const
    {
        if (!integerScaleActive)
            return false;
        
        if (integerScaleFactor.x < 1 || integerScaleFactor.y < 1) // XXX should be < 2, this is for testing only
            return false;
        
        return true;
    }
    
    /* ARC: one line in _log/engine_gfx.txt about the window / drawing box */
    void arcLogResize(const char *why) {
        if (arcGfx.evtLines >= ARC_EVT_LINES_MAX)
            return;
        
        ++arcGfx.evtLines;
        
        char clk[16];
        arcLogLine(2, ARC_GFX_LOG, 1000000,
                   "[%s 엔진 %.1f초] 창 크기 계산(%s) | 창 %dx%d · 게임 화면 %dx%d · 그리는 칸 %dx%d @(%d,%d) · 비율고정 %d",
                   arcClockStr(clk), SDL_GetTicks() / 1000.0, why,
                   winSize.x, winSize.y, scRes.x, scRes.y, scSize.x, scSize.y,
                   scOffset.x, scOffset.y, threadData->config.fixedAspectRatio ? 1 : 0);
    }
    
    void applyWindowSize(const Vec2i &drawableSize, float newBackingScale,
                         bool skipIntScaleBuffer, const char *why) {
        backingScaleFactor = newBackingScale;
        winSize = drawableSize;
        
        /* Make sure integer buffers are rebuilt before screen offsets are
         * calculated so we have the final allocated buffer size ready */
        if (integerScaleActive && findHighestIntegerScale() && !skipIntScaleBuffer)
            rebuildIntegerScaleBuffer();
        
        /* some GL drivers change the viewport on window resize */
        glState.viewport.refresh();
        recalculateScreenSize(threadData->config.fixedAspectRatio);
        updateScreenResoRatio(threadData);
        
        SDL_Rect screen = {scOffset.x, winSize.y - scOffset.y - scSize.y, scSize.x, scSize.y};
        threadData->ethread->notifyGameScreenChange(screen);
        
        arcLogResize(why);
    }
    
    void checkResize(bool skipIntScaleBuffer = false) {
        if (threadData->windowSizeMsg.poll(winSize)) {
            /* Query the actual size in pixels, not units */
            Vec2i drawableSize(winSize);
            threadData->drawableSizeMsg.poll(drawableSize);
            
            applyWindowSize(drawableSize, drawableSize.x / winSize.x,
                            skipIntScaleBuffer, "창 크기 신호");
        }
        
#ifdef __ANDROID__
        /* ARC: on the phone the engine is never told the real size of the
         * screen unless the phone is rotated (until then winSize is only the
         * default from mkxp.json), so the picture was drawn into a box made
         * for the wrong size.  Ask the window itself - it costs nothing. */
        {
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(threadData->window, &dw, &dh);
            
            if (dw > 0 && dh > 0 && (dw != winSize.x || dh != winSize.y))
                applyWindowSize(Vec2i(dw, dh), 1, skipIntScaleBuffer, "창 크기 직접 확인");
        }
#endif
    }
    
    void checkShutDownReset() {
        shState->checkShutdown();
        shState->checkReset();
    }
    
    void shutdown() {
        threadData->rqTermAck.set();
        shState->texPool().disable();
        
        scriptBinding->terminate();
    }
    
    /* ARC: first frame - write what we are running on / how it is set up */
    void arcLogHeader() {
        arcGfx.headerDone = true;
        
        char clk[16];
        const Config &c = threadData->config;
        
        arcLogLine(2, ARC_GFX_LOG, 1000000, "===== %s ARC engine gfx profile (v1.0.13)", arcClockStr(clk));
        arcLogLine(2, ARC_GFX_LOG, 1000000, "GL_VENDOR=%s | GL_RENDERER=%s | GL_VERSION=%s",
                   (const char *) gl.GetString(GL_VENDOR),
                   (const char *) gl.GetString(GL_RENDERER),
                   (const char *) gl.GetString(GL_VERSION));
        arcLogLine(2, ARC_GFX_LOG, 1000000, "GPU타이머(EXT_disjoint_timer_query): %s",
                   arcGfx.gpu.ok ? "사용" : "이 폰 드라이버에 없음");
        arcLogLine(2, ARC_GFX_LOG, 1000000,
                   "옵션: frameSkip(mkxp.json)=%d frameSkip(지금)=%d · 비율고정=%d · 부드럽게확대=%d · vsync=%d(지금 스왑간격 %d) · syncToRefreshrate=%d · fixedFramerate=%d · 한도기 %s · 화면주사율 %d · 게임프레임 %d",
                   c.frameSkip ? 1 : 0, useFrameSkip ? 1 : 0, c.fixedAspectRatio ? 1 : 0,
                   c.smoothScaling ? 1 : 0, c.vsync ? 1 : 0, SDL_GL_GetSwapInterval(),
                   c.syncToRefreshrate ? 1 : 0, c.fixedFramerate,
                   fpsLimiter.disabled ? "꺼짐" : "켜짐", threadData->refreshRate, frameRate);
        arcLogLine(2, ARC_GFX_LOG, 1000000,
                   "화면: 창 %dx%d · 게임 화면 %dx%d · 그리는 칸 %dx%d @(%d,%d) (mkxp.json 기본 %dx%d)",
                   winSize.x, winSize.y, scRes.x, scRes.y, scSize.x, scSize.y,
                   scOffset.x, scOffset.y, c.defScreenW, c.defScreenH);
    }
    
    void swapGLBuffer() {
        uint64_t arcT0 = SDL_GetPerformanceCounter();
        arcGfx.swapStart(arcT0);
        
        fpsLimiter.delay();
        
        uint64_t arcT1 = SDL_GetPerformanceCounter();
        
        SDL_GL_SwapWindow(threadData->window);
        
        uint64_t arcT2 = SDL_GetPerformanceCounter();
        
        ++frameCount;
        
        threadData->ethread->notifyFrame();
        
        arcGfx.useFrameSkipNow = useFrameSkip;
        
        if (!arcGfx.headerDone)
            arcLogHeader();
        
        arcGfx.frameEnd(arcT2, arcTicksToMs(arcT1 - arcT0), arcTicksToMs(arcT2 - arcT1));
    }
    
    void compositeToBuffer(TEXFBO &buffer) {
        screen.composite();
        
        GLMeta::blitBegin(buffer);
        GLMeta::blitSource(screen.getPP().frontBuffer());
        GLMeta::blitRectangle(IntRect(0, 0, scRes.x, scRes.y), Vec2i());
        GLMeta::blitEnd();
    }
    
    void metaBlitBufferFlippedScaled() {
        metaBlitBufferFlippedScaled(scRes);
        GLMeta::blitRectangle(
                              IntRect(0, 0, scRes.x, scRes.y),
                              IntRect(scOffset.x,
                                      (scSize.y + scOffset.y),
                                      scSize.x,
                                      -scSize.y),
                              threadData->config.smoothScaling);
    }
    
    void metaBlitBufferFlippedScaled(const Vec2i &sourceSize, bool forceNearestNeighbor=false) {
        GLMeta::blitRectangle(IntRect(0, 0, sourceSize.x, sourceSize.y),
                              IntRect(scOffset.x, scSize.y+scOffset.y, scSize.x, -scSize.y),
                              !forceNearestNeighbor && threadData->config.smoothScaling);
    }
    
    void redrawScreen() {
        arcGfx.drawBegin();
        screen.composite();
        arcGfx.compositeEnd();
        
        // maybe unspaghetti this later
        if (integerScaleStepApplicable() && !integerLastMileScaling)
        {
            GLMeta::blitBeginScreen(winSize);
            GLMeta::blitSource(screen.getPP().frontBuffer());
            
            FBO::clear();
            metaBlitBufferFlippedScaled(scRes, true);
            GLMeta::blitEnd();
            
            swapGLBuffer();
            return;
        }
        
        if (integerScaleStepApplicable())
        {
            assert(integerScaleBuffer.tex != TEX::ID(0));
            GLMeta::blitBegin(integerScaleBuffer);
            GLMeta::blitSource(screen.getPP().frontBuffer());
            
            GLMeta::blitRectangle(IntRect(0, 0, scRes.x, scRes.y),
                                  IntRect(0, 0, integerScaleBuffer.width, integerScaleBuffer.height),
                                  false);
            
            GLMeta::blitEnd();
        }
        
        GLMeta::blitBeginScreen(winSize);
        //GLMeta::blitSource(screen.getPP().frontBuffer());
        
        Vec2i sourceSize;
        
        if (integerScaleActive)
        {
            GLMeta::blitSource(integerScaleBuffer);
            sourceSize = Vec2i(integerScaleBuffer.width, integerScaleBuffer.height);
        }
        else
        {
            GLMeta::blitSource(screen.getPP().frontBuffer());
            sourceSize = scRes;
        }
        
        FBO::clear();
        metaBlitBufferFlippedScaled(sourceSize);
        
        GLMeta::blitEnd();
        
        swapGLBuffer();
        
        SDL_LockMutex(avgFPSLock);
        if (avgFPSData.size() > 40)
            avgFPSData.erase(avgFPSData.begin());
        
        unsigned long long time = shState->runTime();
        avgFPSData.push_back(time - last_avg_update);
        last_avg_update = time;
        SDL_UnlockMutex(avgFPSLock);
    }
    
    void checkSyncLock() {
        if (!threadData->syncPoint.mainSyncLocked())
            return;
        
        /* Releasing the GL context before sleeping and making it
         * current again on wakeup seems to avoid the context loss
         * when the app moves into the background on Android */
        SDL_GL_MakeCurrent(threadData->window, 0);
        threadData->syncPoint.waitMainSync();
        SDL_GL_MakeCurrent(threadData->window, glCtx);
        
        fpsLimiter.resetFrameAdjust();
    }
    
    double averageFPS() {
        double ret = 0;
        SDL_LockMutex(avgFPSLock);
        for (unsigned long long times : avgFPSData)
            ret += times;
        
        ret = 1 / (ret / avgFPSData.size() / 1000000);
        SDL_UnlockMutex(avgFPSLock);
        return ret;
    }
    
    void setLock(bool force = false) {
        if (!(force || multithreadedMode)) return;
        
        SDL_LockMutex(glResourceLock);
        SDL_GL_MakeCurrent(threadData->window, threadData->glContext);
    }
    
    void releaseLock(bool force = false) {
        if (!(force || multithreadedMode)) return;
        
        SDL_UnlockMutex(glResourceLock);
    }
};

Graphics::Graphics(RGSSThreadData *data) {
    p = new GraphicsPrivate(data);
    if (data->config.syncToRefreshrate) {
        p->frameRate = data->refreshRate;
        p->fpsLimiter.disabled = true;
    } else if (data->config.fixedFramerate > 0) {
        p->fpsLimiter.setDesiredFPS(data->config.fixedFramerate);
    } else if (data->config.fixedFramerate < 0) {
        p->fpsLimiter.disabled = true;
    }
}

Graphics::~Graphics() { delete p; }

unsigned long long Graphics::getDelta() {
    return shState->runTime() - p->last_update;
}

unsigned long long Graphics::lastUpdate() {
    return p->last_update;
}

void Graphics::update(bool checkForShutdown) {
    p->threadData->rqWindowAdjust.wait();
    p->last_update = shState->runTime();
    
    if (checkForShutdown)
        p->checkShutDownReset();
    
    p->checkSyncLock();
    
    
#ifdef MKXPZ_STEAM
    if (STEAMSHIM_alive())
        STEAMSHIM_pump();
#endif
    
    if (p->frozen)
        return;
    
    if (p->fpsLimiter.frameSkipRequired()) {
        if (p->useFrameSkip) {
            /* Skip frame */
            uint64_t arcS0 = SDL_GetPerformanceCounter();
            p->fpsLimiter.delay();
            arcGfx.skipFrame(arcTicksToMs(SDL_GetPerformanceCounter() - arcS0));
            ++p->frameCount;
            p->threadData->ethread->notifyFrame();
            
            return;
        } else {
            /* Just reset frame adjust counter */
            p->fpsLimiter.resetFrameAdjust();
        }
    }
    
    p->checkResize();
    p->redrawScreen();
}

void Graphics::freeze() {
    p->frozen = true;
    
    p->checkShutDownReset();
    p->checkResize();
    
    /* Capture scene into frozen buffer */
    p->compositeToBuffer(p->frozenScene);
}

void Graphics::transition(int duration, const char *filename, int vague) {
    p->checkSyncLock();
    
    if (!p->frozen)
        return;
    
    vague = clamp(vague, 1, 256);
    Bitmap *transMap = *filename ? new Bitmap(filename) : 0;
    
    setBrightness(255);
    
    /* Capture new scene */
    p->screen.composite();
    
    /* The PP frontbuffer will hold the current scene after the
     * composition step. Since the backbuffer is unused during
     * the transition, we can reuse it as the target buffer for
     * the final rendered image. */
    TEXFBO &currentScene = p->screen.getPP().frontBuffer();
    TEXFBO &transBuffer = p->screen.getPP().backBuffer();
    
    /* If no transition bitmap is provided,
     * we can use a simplified shader */
    TransShader &transShader = shState->shaders().trans;
    SimpleTransShader &simpleShader = shState->shaders().simpleTrans;
    
    if (transMap) {
        TransShader &shader = transShader;
        shader.bind();
        shader.applyViewportProj();
        shader.setFrozenScene(p->frozenScene.tex);
        shader.setCurrentScene(currentScene.tex);
        shader.setTransMap(transMap->getGLTypes().tex);
        shader.setVague(vague / 256.0f);
        shader.setTexSize(p->scRes);
    } else {
        SimpleTransShader &shader = simpleShader;
        shader.bind();
        shader.applyViewportProj();
        shader.setFrozenScene(p->frozenScene.tex);
        shader.setCurrentScene(currentScene.tex);
        shader.setTexSize(p->scRes);
    }
    
    glState.blend.pushSet(false);
    
    for (int i = 0; i < duration; ++i) {
        /* We need to clean up transMap properly before
         * a possible longjmp, so we manually test for
         * shutdown/reset here */
        if (p->threadData->rqTerm) {
            glState.blend.pop();
            delete transMap;
            p->shutdown();
            return;
        }
        
        if (p->threadData->rqReset) {
            glState.blend.pop();
            delete transMap;
            scriptBinding->reset();
            return;
        }
        
        p->checkSyncLock();
        
        const float prog = i * (1.0f / duration);
        
        if (transMap) {
            transShader.bind();
            transShader.setProg(prog);
        } else {
            simpleShader.bind();
            simpleShader.setProg(prog);
        }
        
        /* Draw the composed frame to a buffer first
         * (we need this because we're skipping PingPong) */
        FBO::bind(transBuffer.fbo);
        FBO::clear();
        p->screenQuad.draw();
        
        p->checkResize();
        
        /* Then blit it flipped and scaled to the screen */
        FBO::unbind();
        FBO::clear();
        
        GLMeta::blitBeginScreen(Vec2i(p->winSize));
        GLMeta::blitSource(transBuffer);
        p->metaBlitBufferFlippedScaled();
        GLMeta::blitEnd();
        
        p->swapGLBuffer();
    }
    
    glState.blend.pop();
    
    delete transMap;
    
    p->frozen = false;
}

void Graphics::frameReset() {p->fpsLimiter.resetFrameAdjust();}

static void guardDisposed() {}

DEF_ATTR_RD_SIMPLE(Graphics, FrameRate, int, p->frameRate)

DEF_ATTR_SIMPLE(Graphics, FrameCount, int, p->frameCount)

void Graphics::setFrameRate(int value) {
    p->frameRate = clamp(value, 10, 120);
    
    if (p->threadData->config.syncToRefreshrate)
        return;
    
    if (p->threadData->config.fixedFramerate > 0)
        return;
    
    p->fpsLimiter.setDesiredFPS(p->frameRate);
    shState->input().recalcRepeat((unsigned int)p->frameRate);
}

double Graphics::averageFrameRate() {
    return p->averageFPS();
}

void Graphics::wait(int duration) {
    for (int i = 0; i < duration; ++i) {
        p->checkShutDownReset();
        p->redrawScreen();
    }
}

void Graphics::fadeout(int duration) {
    FBO::unbind();
    
    float curr = p->brightness;
    float diff = 255.0f - curr;
    
    for (int i = duration - 1; i > -1; --i) {
        setBrightness(diff + (curr / duration) * i);
        
        if (p->frozen) {
            GLMeta::blitBeginScreen(p->scSize);
            GLMeta::blitSource(p->frozenScene);
            
            FBO::clear();
            p->metaBlitBufferFlippedScaled();
            
            GLMeta::blitEnd();
            
            p->swapGLBuffer();
        } else {
            update();
        }
    }
}

void Graphics::fadein(int duration) {
    FBO::unbind();
    
    float curr = p->brightness;
    float diff = 255.0f - curr;
    
    for (int i = 1; i <= duration; ++i) {
        setBrightness(curr + (diff / duration) * i);
        
        if (p->frozen) {
            GLMeta::blitBeginScreen(p->scSize);
            GLMeta::blitSource(p->frozenScene);
            
            FBO::clear();
            p->metaBlitBufferFlippedScaled();
            
            GLMeta::blitEnd();
            
            p->swapGLBuffer();
        } else {
            update();
        }
    }
}

Bitmap *Graphics::snapToBitmap() {
    Bitmap *bitmap = new Bitmap(width(), height());
    
    p->compositeToBuffer(bitmap->getGLTypes());
    
    /* Taint entire bitmap */
    bitmap->taintArea(IntRect(0, 0, width(), height()));
    return bitmap;
}

int Graphics::width() const { return p->scRes.x; }

int Graphics::height() const { return p->scRes.y; }

void Graphics::resizeScreen(int width, int height) {
    p->threadData->rqWindowAdjust.wait();
    p->checkResize(true);
    
    Vec2i size(width, height);
    
    if (p->scRes == size)
        return;
    
    p->scRes = size;
    
    p->screen.setResolution(width, height);
    
    if (p->integerScaleActive)
        p->rebuildIntegerScaleBuffer();
    
    TEXFBO::allocEmpty(p->frozenScene, width, height);
    
    FloatRect screenRect(0, 0, width, height);
    p->screenQuad.setTexPosRect(screenRect, screenRect);
    
    glState.scissorBox.set(IntRect(0, 0, p->scRes.x, p->scRes.y));
    
    shState->eThread().requestWindowResize(width, height);
    
    /* ARC: the box the picture is drawn into depends on the aspect ratio of
     * the game screen (682x384 is 16:9, the start is 640x480 = 4:3).  On the
     * phone the window never answers requestWindowResize with a "size
     * changed" signal, so the old 4:3 box stayed and the 16:9 picture was
     * squeezed into it.  Work the box out again right now. */
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
    
    {
        SDL_Rect screen = {p->scOffset.x, p->winSize.y - p->scOffset.y - p->scSize.y, p->scSize.x, p->scSize.y};
        p->threadData->ethread->notifyGameScreenChange(screen);
    }
    
    p->arcLogResize("해상도 바꿈");
}

void Graphics::resizeWindow(int width, int height, bool center) {
    p->threadData->rqWindowAdjust.wait();
    p->checkResize();
    
    if (width == p->winSize.x / p->backingScaleFactor &&
        height == p->winSize.y / p->backingScaleFactor)
            return;

    shState->eThread().requestWindowResize(width, height);
    
    if (center)
        this->center();
}

bool Graphics::updateMovieInput(Movie *movie) {
    return  p->threadData->rqTerm || p->threadData->rqReset;
}

void Graphics::playMovie(const char *filename, int volume_, bool skippable) {
    Movie *movie = new Movie(skippable);
    MovieOpenHandler handler(movie->srcOps);
    shState->fileSystem().openRead(handler, filename);
    float volume = volume_ * 0.01f;
    
    if (movie->preparePlayback()) {        
        Sprite movieSprite;
        
        // Currently this stretches to fit the screen. VX Ace behavior is to center it and let the edges run off
        movieSprite.setBitmap(movie->videoBitmap);
        double ratio = std::min((double)width() / movie->video->width, (double)height() / movie->video->height);
        movieSprite.setZoomX(ratio);
        movieSprite.setZoomY(ratio);
        movieSprite.setX((width() / 2) - (movie->video->width * ratio / 2));
        movieSprite.setY((height() / 2) - (movie->video->height * ratio / 2));
        
        Sprite letterboxSprite;
        Bitmap letterbox(width(), height());
        letterbox.fillRect(0, 0, width(), height(), Vec4(0,0,0,255));
        letterboxSprite.setBitmap(&letterbox);
        
        letterboxSprite.setZ(4999);
        movieSprite.setZ(5001);
        
        movie->play(volume);
    }
    
    delete movie;
}

void Graphics::screenshot(const char *filename) {
    p->threadData->rqWindowAdjust.wait();
    Bitmap *ss = snapToBitmap();
    ss->saveToFile(filename);
    ss->dispose();
    delete ss;
}

DEF_ATTR_RD_SIMPLE(Graphics, Brightness, int, p->brightness)

void Graphics::setBrightness(int value) {
    value = clamp(value, 0, 255);
    
    if (p->brightness == value)
        return;
    
    p->brightness = value;
    p->screen.setBrightness(value / 255.0);
}

void Graphics::reset() {
    /* Dispose all live Disposables */
    IntruListLink<Disposable> *iter;
    
    for (iter = p->dispList.begin(); iter != p->dispList.end();
         iter = iter->next) {
        iter->data->dispose();
    }
    
    p->dispList.clear();
    
    /* Reset attributes (frame count not included) */
    p->fpsLimiter.resetFrameAdjust();
    p->frozen = false;
    p->screen.getPP().clearBuffers();
    
    setFrameRate(DEF_FRAMERATE);
    setBrightness(255);
}

void Graphics::center() {
    p->threadData->rqWindowAdjust.wait();
    if (getFullscreen())
        return;
    
    p->threadData->ethread->requestWindowCenter();
}

bool Graphics::getFullscreen() const {
    return p->threadData->ethread->getFullscreen();
}

void Graphics::setFullscreen(bool value) {
    p->threadData->ethread->requestFullscreenMode(value);
}

bool Graphics::getShowCursor() const {
    return p->threadData->ethread->getShowCursor();
}

void Graphics::setShowCursor(bool value) {
    p->threadData->ethread->requestShowCursor(value);
}

bool Graphics::getFixedAspectRatio() const
{
    // It's a bit hacky to expose config values as a Graphics
    // attribute, but there's really no point in state duplication
    return shState->config().fixedAspectRatio;
}

void Graphics::setFixedAspectRatio(bool value)
{
    shState->config().fixedAspectRatio = value;
    p->recalculateScreenSize(p->threadData);
    p->findHighestIntegerScale();
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
}

bool Graphics::getSmoothScaling() const
{
    // Same deal as with fixed aspect ratio
    return shState->config().smoothScaling;
}

void Graphics::setSmoothScaling(bool value)
{
    shState->config().smoothScaling = value;
}

bool Graphics::getIntegerScaling() const
{
    return p->integerScaleActive;
}

void Graphics::setIntegerScaling(bool value)
{
    p->integerScaleActive = value;
    p->findHighestIntegerScale();
    p->rebuildIntegerScaleBuffer();
    
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
}

bool Graphics::getLastMileScaling() const
{
    return p->integerLastMileScaling;
}

void Graphics::setLastMileScaling(bool value)
{
    p->integerLastMileScaling = value;
    p->recalculateScreenSize(p->threadData->config.fixedAspectRatio);
    p->updateScreenResoRatio(p->threadData);
}

bool Graphics::getThreadsafe() const
{
    return p->multithreadedMode;
}

void Graphics::setThreadsafe(bool value)
{
    p->multithreadedMode = value;
}

double Graphics::getScale() const {
    p->checkResize();
    return (double)(p->winSize.y / p->backingScaleFactor) / p->scRes.y;
    
}

void Graphics::setScale(double factor) {
    p->threadData->rqWindowAdjust.wait();
    factor = clamp(factor, 0.5, 4.0);
    
    if (factor == getScale())
        return;
    
    int widthpx = p->scRes.x * factor;
    int heightpx = p->scRes.y * factor;
    
    shState->eThread().requestWindowResize(widthpx, heightpx);
}

bool Graphics::getFrameskip() const { return p->useFrameSkip; }

void Graphics::setFrameskip(bool value) { p->useFrameSkip = value; }

Scene *Graphics::getScreen() const { return &p->screen; }

void Graphics::repaintWait(const AtomicFlag &exitCond, bool checkReset) {
    if (exitCond)
        return;
    
    /* Repaint the screen with the last good frame we drew */
    TEXFBO &lastFrame = p->screen.getPP().frontBuffer();
    GLMeta::blitBeginScreen(p->winSize);
    GLMeta::blitSource(lastFrame);
    
    while (!exitCond) {
        shState->checkShutdown();
        
        if (checkReset)
            shState->checkReset();
        
        FBO::clear();
        p->metaBlitBufferFlippedScaled();
        SDL_GL_SwapWindow(p->threadData->window);
        p->fpsLimiter.delay();
        
        p->threadData->ethread->notifyFrame();
    }
    
    GLMeta::blitEnd();
}

void Graphics::lock(bool force) {
    p->setLock(force);
}

void Graphics::unlock(bool force) {
    p->releaseLock(force);
}

void Graphics::addDisposable(Disposable *d) { p->dispList.append(d->link); }

void Graphics::remDisposable(Disposable *d) { p->dispList.remove(d->link); }

#undef GRAPHICS_THREAD_LOCK
