#include "tracker.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <Vision/Vision.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace mh {
namespace {

// MediaPipe landmark order, which core/ expects.
NSArray<VNHumanHandPoseObservationJointName>* jointOrder() {
  static NSArray* order = @[
    VNHumanHandPoseObservationJointNameWrist,
    VNHumanHandPoseObservationJointNameThumbCMC, VNHumanHandPoseObservationJointNameThumbMP,
    VNHumanHandPoseObservationJointNameThumbIP, VNHumanHandPoseObservationJointNameThumbTip,
    VNHumanHandPoseObservationJointNameIndexMCP, VNHumanHandPoseObservationJointNameIndexPIP,
    VNHumanHandPoseObservationJointNameIndexDIP, VNHumanHandPoseObservationJointNameIndexTip,
    VNHumanHandPoseObservationJointNameMiddleMCP, VNHumanHandPoseObservationJointNameMiddlePIP,
    VNHumanHandPoseObservationJointNameMiddleDIP, VNHumanHandPoseObservationJointNameMiddleTip,
    VNHumanHandPoseObservationJointNameRingMCP, VNHumanHandPoseObservationJointNameRingPIP,
    VNHumanHandPoseObservationJointNameRingDIP, VNHumanHandPoseObservationJointNameRingTip,
    VNHumanHandPoseObservationJointNameLittleMCP, VNHumanHandPoseObservationJointNameLittlePIP,
    VNHumanHandPoseObservationJointNameLittleDIP, VNHumanHandPoseObservationJointNameLittleTip,
  ];
  return order;
}

double hostSeconds() {
  static mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return t;
  }();
  return double(mach_absolute_time()) * tb.numer / tb.denom / 1e9;
}

// Plane 0 of the 420v frames is luma; the caller must hold the base address lock.
LumaView lumaOf(CVPixelBufferRef pixels) {
  LumaView v;
  v.data = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
  v.width = int(CVPixelBufferGetWidthOfPlane(pixels, 0));
  v.height = int(CVPixelBufferGetHeightOfPlane(pixels, 0));
  v.stride = int(CVPixelBufferGetBytesPerRowOfPlane(pixels, 0));
  v.native = pixels;
  return v;
}

VNDetectHumanHandPoseRequest* makeRequest() {
  VNDetectHumanHandPoseRequest* request = [[VNDetectHumanHandPoseRequest alloc] init];
  request.maximumHandCount = 2;
  return request;
}

// Converts Vision observations (origin bottom-left, camera view) into
// mirrored top-left coordinates, which is how performers see themselves.
std::vector<Detection> toDetections(NSArray<VNHumanHandPoseObservation*>* observations) {
  std::vector<Detection> out;
  NSArray* joints = jointOrder();
  for (VNHumanHandPoseObservation* obs in observations) {
    NSDictionary<VNHumanHandPoseObservationJointName, VNRecognizedPoint*>* points =
        [obs recognizedPointsForJointsGroupName:VNHumanHandPoseObservationJointsGroupNameAll error:nil];
    if (!points) continue;
    Detection d;
    d.confidence = obs.confidence;
    d.chirality = obs.chirality == VNChiralityLeft ? Left : obs.chirality == VNChiralityRight ? Right : -1;
    bool complete = true;
    for (NSUInteger i = 0; i < joints.count; ++i) {
      VNRecognizedPoint* p = points[joints[i]];
      if (!p || p.confidence <= 0.f) {
        complete = false;
        break;
      }
      d.lm[i] = {static_cast<float>(1.0 - p.location.x), static_cast<float>(1.0 - p.location.y)};
    }
    if (complete) out.push_back(d);
  }
  return out;
}

NSArray<AVCaptureDevice*>* captureDevices() {
  NSArray* types = @[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal ];
  AVCaptureDeviceDiscoverySession* discovery =
      [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                             mediaType:AVMediaTypeVideo
                                                              position:AVCaptureDevicePositionUnspecified];
  return discovery.devices;
}

// Highest frame rate first (it bounds latency), then the size closest to
// 1280x720: big enough for small hands, small enough to move quickly.
AVCaptureDeviceFormat* bestFormat(AVCaptureDevice* device, double* fpsOut, CMTime* frameDuration = nullptr) {
  AVCaptureDeviceFormat* best = nil;
  AVFrameRateRange* bestRange = nil;
  double bestFps = 0.0, bestCost = 1e18;
  for (AVCaptureDeviceFormat* format in device.formats) {
    AVFrameRateRange* fastest = nil;
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
      if (!fastest || range.maxFrameRate > fastest.maxFrameRate) fastest = range;
    const double fps = fastest ? fastest.maxFrameRate : 0.0;
    const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    const double cost = std::fabs(double(dim.width) * dim.height - 1280.0 * 720.0);
    if (fps > bestFps + 0.5 || (std::fabs(fps - bestFps) <= 0.5 && cost < bestCost)) {
      best = format;
      bestRange = fastest;
      bestFps = fps;
      bestCost = cost;
    }
  }
  if (fpsOut) *fpsOut = bestFps;
  // The camera only accepts durations it reports itself, exactly.
  if (frameDuration && bestRange) *frameDuration = bestRange.minFrameDuration;
  return best;
}

CameraInfo describe(AVCaptureDevice* device) {
  CameraInfo info;
  info.name = device.localizedName.UTF8String;
  info.uid = device.uniqueID.UTF8String;
  if (AVCaptureDeviceFormat* format = bestFormat(device, &info.fps)) {
    const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    info.width = dim.width;
    info.height = dim.height;
  }
  return info;
}

}  // namespace
}  // namespace mh

@interface MHCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) mh::DetectionCallback callback;
@end

@implementation MHCaptureDelegate {
  VNSequenceRequestHandler* _handler;
  VNDetectHumanHandPoseRequest* _request;
  double _lastFrame;
  double _fps;
}

- (instancetype)init {
  if ((self = [super init])) {
    _handler = [[VNSequenceRequestHandler alloc] init];
    _request = mh::makeRequest();
  }
  return self;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
  CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!pixels || !_callback) return;
  const double frameTime = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sampleBuffer));

  const double t0 = mh::hostSeconds();
  NSError* error = nil;
  [_handler performRequests:@[ _request ] onCVPixelBuffer:pixels orientation:kCGImagePropertyOrientationUp error:&error];
  const double t1 = mh::hostSeconds();

  mh::TrackerStats stats;
  if (_lastFrame > 0.0 && frameTime > _lastFrame) {
    const double instant = 1.0 / (frameTime - _lastFrame);
    _fps = _fps > 0.0 ? _fps + 0.1 * (instant - _fps) : instant;
  }
  _lastFrame = frameTime;
  stats.fps = _fps;
  stats.detectMs = (t1 - t0) * 1000.0;
  // Capture timestamps are on the host clock, so this includes driver and
  // queueing delay, not just Vision.
  stats.latencyMs = (t1 - frameTime) * 1000.0;

  const float aspect = float(CVPixelBufferGetWidth(pixels)) / float(std::max<size_t>(1, CVPixelBufferGetHeight(pixels)));
  std::vector<mh::Detection> hands = error ? std::vector<mh::Detection>{} : mh::toDetections(_request.results);
  CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  _callback(hands, frameTime, aspect, stats, mh::lumaOf(pixels));
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
}

@end

namespace mh {

std::vector<CameraInfo> listCameras() {
  std::vector<CameraInfo> out;
  for (AVCaptureDevice* device in captureDevices()) out.push_back(describe(device));
  return out;
}

CameraAccess cameraAccess() {
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized:
      return CameraAccess::Granted;
    case AVAuthorizationStatusNotDetermined:
      return CameraAccess::Undetermined;
    default:
      return CameraAccess::Denied;
  }
}

void requestCameraAccess(std::function<void(bool)> done) {
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL granted) {
                             if (done) done(granted);
                           }];
}

struct Tracker::Impl {
  AVCaptureSession* session = nil;
  MHCaptureDelegate* delegate = nil;
  dispatch_queue_t queue = nil;
  CameraInfo camera;
  mutable std::mutex mutex;
};

Tracker::Tracker() : impl_(std::make_unique<Impl>()) {}
Tracker::~Tracker() { stop(); }

bool Tracker::start(const std::string& camera, DetectionCallback callback, std::string* error) {
  stop();
  @autoreleasepool {
    AVCaptureDevice* device = nil;
    for (AVCaptureDevice* candidate in captureDevices()) {
      if (camera.empty() || camera == candidate.uniqueID.UTF8String || camera == candidate.localizedName.UTF8String) {
        device = candidate;
        break;
      }
    }
    if (!device) {
      if (error) *error = camera.empty() ? "no camera found" : "camera not found: " + camera;
      return false;
    }

    NSError* nsError = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&nsError];
    if (!input) {
      if (error) *error = nsError ? nsError.localizedDescription.UTF8String : "cannot open camera";
      return false;
    }

    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    [session beginConfiguration];
    if (![session canAddInput:input]) {
      if (error) *error = "camera is busy";
      return false;
    }
    [session addInput:input];

    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    // Native camera format: no color conversion before Vision.
    output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    // Never queue stale frames: latency matters more than completeness.
    output.alwaysDiscardsLateVideoFrames = YES;
    dispatch_queue_attr_t attr =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    dispatch_queue_t queue = dispatch_queue_create("midihands.capture", attr);
    MHCaptureDelegate* delegate = [[MHCaptureDelegate alloc] init];
    delegate.callback = std::move(callback);
    [output setSampleBufferDelegate:delegate queue:queue];
    if (![session canAddOutput:output]) {
      if (error) *error = "cannot read frames from camera";
      return false;
    }
    [session addOutput:output];

    // Keep the device locked through startRunning, otherwise the session
    // replaces our format with its own preset.
    double fps = 0.0;
    CMTime frame = kCMTimeInvalid;
    AVCaptureDeviceFormat* format = bestFormat(device, &fps, &frame);
    const bool locked = format && [device lockForConfiguration:nil];
    if (locked) {
      device.activeFormat = format;
      if (CMTIME_IS_VALID(frame)) {
        @try {
          device.activeVideoMinFrameDuration = frame;
          device.activeVideoMaxFrameDuration = frame;
        } @catch (NSException*) {
          // Some drivers reject explicit rates; the format default still works.
        }
      }
    }
    [session commitConfiguration];
    [session startRunning];
    if (locked) [device unlockForConfiguration];

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->session = session;
    impl_->delegate = delegate;
    impl_->queue = queue;
    impl_->camera = describe(device);
  }
  return true;
}

void Tracker::stop() {
  AVCaptureSession* session = nil;
  dispatch_queue_t queue = nil;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    session = impl_->session;
    queue = impl_->queue;
    impl_->session = nil;
    impl_->delegate = nil;
    impl_->queue = nil;
  }
  if (session) [session stopRunning];
  // Wait for an in-flight frame to finish so its callback cannot outlive us.
  if (queue) dispatch_sync(queue, ^{});
}

bool Tracker::running() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->session != nil;
}

CameraInfo Tracker::camera() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->camera;
}

bool processMovie(
    const std::string& path,
    const std::function<void(const std::vector<Detection>&, double, float, double, const LumaView&)>& cb,
    std::string* error) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    NSError* nsError = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&nsError];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    AVAssetTrack* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
#pragma clang diagnostic pop
    if (!reader || !track) {
      if (error) *error = "cannot read movie: " + path;
      return false;
    }
    AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:track
                         outputSettings:@{
                           (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                         }];
    output.alwaysCopiesSampleData = NO;
    [reader addOutput:output];
    [reader startReading];

    VNSequenceRequestHandler* handler = [[VNSequenceRequestHandler alloc] init];
    VNDetectHumanHandPoseRequest* request = makeRequest();
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
      @autoreleasepool {
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
        const double time = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
        const double t0 = hostSeconds();
        [handler performRequests:@[ request ] onCVPixelBuffer:pixels orientation:kCGImagePropertyOrientationUp error:nil];
        const double ms = (hostSeconds() - t0) * 1000.0;
        const float aspect = float(CVPixelBufferGetWidth(pixels)) / float(std::max<size_t>(1, CVPixelBufferGetHeight(pixels)));
        CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        cb(toDetections(request.results), time, aspect, ms, lumaOf(pixels));
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
      }
      CFRelease(sample);
    }
  }
  return true;
}

}  // namespace mh
