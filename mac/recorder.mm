#include "recorder.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <atomic>
#include <mutex>

using StartedFn = std::function<void(bool, const std::string&)>;

@interface MHRecording : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) SCStream* stream;
@property(nonatomic) AVAssetWriter* writer;
@property(nonatomic) AVAssetWriterInput* video;
@property(nonatomic) AVAssetWriterInput* audio;
@property(nonatomic) NSURL* url;
@property(nonatomic) dispatch_queue_t queue;
@property(atomic) BOOL sessionStarted;
@property(atomic) BOOL stopping;
@end

@implementation MHRecording

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
  if (self.stopping || !CMSampleBufferIsValid(sample)) return;
  if (type == SCStreamOutputTypeScreen) {
    // Only frames with new content; ScreenCaptureKit also sends idle and blank ones.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;
    if (!self.sessionStarted) {
      if (![self.writer startWriting]) return;
      [self.writer startSessionAtSourceTime:CMSampleBufferGetPresentationTimeStamp(sample)];
      self.sessionStarted = YES;
    }
    if (self.video.readyForMoreMediaData) [self.video appendSampleBuffer:sample];
  } else if (type == SCStreamOutputTypeAudio) {
    if (self.sessionStarted && self.audio.readyForMoreMediaData) [self.audio appendSampleBuffer:sample];
  }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  NSLog(@"midihands recorder: stream stopped: %@", error);
}

@end

namespace mh {

namespace {
std::mutex gMutex;
MHRecording* gActive = nil;
std::atomic<bool> gStarting{false};  // between start() and the stream running
std::string gLastFile;

NSURL* newFileURL() {
  NSURL* movies = [[NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory inDomains:NSUserDomainMask] firstObject];
  NSURL* dir = [movies URLByAppendingPathComponent:@"MidiHands" isDirectory:YES];
  [NSFileManager.defaultManager createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:nil];
  NSDateFormatter* f = [[NSDateFormatter alloc] init];
  f.dateFormat = @"yyyy-MM-dd HH.mm.ss";
  return [dir URLByAppendingPathComponent:[NSString stringWithFormat:@"MidiHands %@.mp4", [f stringFromDate:NSDate.date]]];
}
}  // namespace

Recorder& Recorder::shared() {
  static Recorder* r = new Recorder();
  return *r;
}

bool Recorder::recording() const {
  std::lock_guard<std::mutex> lock(gMutex);
  return gActive != nil;
}

std::string Recorder::lastFile() const {
  std::lock_guard<std::mutex> lock(gMutex);
  return gLastFile;
}

void Recorder::start(const std::string& title, double x, double y, double w, double h, StartedFn started) {
  if (recording() || gStarting.exchange(true)) { started(false, "already recording"); return; }
  // Every way out of starting clears gStarting before reporting.
  started = [started](bool ok, const std::string& msg) { gStarting = false; started(ok, msg); };
  if (!CGPreflightScreenCaptureAccess()) {
    CGRequestScreenCaptureAccess();  // shows the system prompt once
    started(false, "allow Screen & System Audio Recording for Ableton Live in System Settings > Privacy & Security, then restart Live");
    return;
  }
  // The window, frontmost first (main thread: AppKit). NSApp.windows, not orderedWindows:
  // the video window floats, and orderedWindows leaves floating windows out.
  NSString* prefix = [NSString stringWithUTF8String:title.c_str()];
  NSWindow* window = nil;
  for (NSWindow* candidate in NSApp.windows)
    if (candidate.isVisible && [candidate.title hasPrefix:prefix] &&
        (!window || candidate.orderedIndex < window.orderedIndex))
      window = candidate;
  if (!window) { started(false, "open the video window first"); return; }
  const CGWindowID windowId = CGWindowID(window.windowNumber);
  const double scale = window.backingScaleFactor;
  const NSRect frame = window.frame, content = window.contentLayoutRect;
  const double titleBar = frame.size.height - (content.origin.y + content.size.height);
  const CGRect source = CGRectMake(x, titleBar + y, w, h);
  // Even pixel sizes for H.264, at most 4K on the long side.
  const double fit = std::min(1.0, 3840.0 / std::max(w, h) / scale);
  const int pw = std::max(2, int(w * scale * fit) & ~1), ph = std::max(2, int(h * scale * fit) & ~1);

  [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
                                               completionHandler:^(SCShareableContent* shareable, NSError* error) {
    if (error || !shareable) {
      started(false, error ? error.localizedDescription.UTF8String : "screen capture is not available");
      return;
    }
    SCWindow* target = nil;
    for (SCWindow* w in shareable.windows)
      if (w.windowID == windowId) { target = w; break; }
    if (!target) { started(false, "the video window is not on screen"); return; }

    MHRecording* rec = [[MHRecording alloc] init];
    rec.queue = dispatch_queue_create("midihands.recorder", DISPATCH_QUEUE_SERIAL);
    rec.url = newFileURL();
    NSError* writerError = nil;
    rec.writer = [AVAssetWriter assetWriterWithURL:rec.url fileType:AVFileTypeMPEG4 error:&writerError];
    if (!rec.writer) { started(false, writerError.localizedDescription.UTF8String); return; }
    const double bitrate = std::min(40e6, std::max(6e6, double(pw) * ph * 60 * 0.11));
    rec.video = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
      AVVideoCodecKey : AVVideoCodecTypeH264, AVVideoWidthKey : @(pw), AVVideoHeightKey : @(ph),
      AVVideoCompressionPropertiesKey : @{
        AVVideoAverageBitRateKey : @(bitrate), AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
        AVVideoExpectedSourceFrameRateKey : @60, AVVideoMaxKeyFrameIntervalKey : @120,
      },
    }];
    rec.video.expectsMediaDataInRealTime = YES;
    rec.audio = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
      AVFormatIDKey : @(kAudioFormatMPEG4AAC), AVSampleRateKey : @48000, AVNumberOfChannelsKey : @2, AVEncoderBitRateKey : @256000,
    }];
    rec.audio.expectsMediaDataInRealTime = YES;
    [rec.writer addInput:rec.video];
    [rec.writer addInput:rec.audio];

    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = size_t(pw);
    config.height = size_t(ph);
    config.sourceRect = source;
    config.minimumFrameInterval = CMTimeMake(1, 60);
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = NO;
    config.queueDepth = 6;
    config.capturesAudio = YES;  // the sound of the window's app: Live
    config.excludesCurrentProcessAudio = NO;
    config.sampleRate = 48000;
    config.channelCount = 2;
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
    rec.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:rec];
    NSError* outputError = nil;
    if (![rec.stream addStreamOutput:rec type:SCStreamOutputTypeScreen sampleHandlerQueue:rec.queue error:&outputError] ||
        ![rec.stream addStreamOutput:rec type:SCStreamOutputTypeAudio sampleHandlerQueue:rec.queue error:&outputError]) {
      started(false, outputError.localizedDescription.UTF8String);
      return;
    }
    [rec.stream startCaptureWithCompletionHandler:^(NSError* startError) {
      if (startError) { started(false, startError.localizedDescription.UTF8String); return; }
      {
        std::lock_guard<std::mutex> lock(gMutex);
        gActive = rec;
      }
      started(true, rec.url.path.UTF8String);
    }];
  }];
}

void Recorder::stop(StartedFn finished) {
  MHRecording* rec = nil;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    rec = gActive;
    gActive = nil;
  }
  if (!rec) { finished(false, "not recording"); return; }
  rec.stopping = YES;
  [rec.stream stopCaptureWithCompletionHandler:^(NSError*) {
    dispatch_async(rec.queue, ^{
      if (!rec.sessionStarted) {
        [rec.writer cancelWriting];
        finished(false, "no picture was captured");
        return;
      }
      [rec.video markAsFinished];
      [rec.audio markAsFinished];
      [rec.writer finishWritingWithCompletionHandler:^{
        if (rec.writer.status == AVAssetWriterStatusCompleted) {
          {
            std::lock_guard<std::mutex> lock(gMutex);
            gLastFile = rec.url.path.UTF8String;
          }
          finished(true, rec.url.path.UTF8String);
        } else {
          finished(false, rec.writer.error ? rec.writer.error.localizedDescription.UTF8String : "the file could not be written");
        }
      }];
    });
  }];
}

}  // namespace mh
