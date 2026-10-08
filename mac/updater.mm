#include "updater.hpp"

#import <Foundation/Foundation.h>

#include <mutex>

#include "../core/version.hpp"

namespace mh {

namespace {
std::mutex gCacheMutex;
UpdateCheck gCached;
double gCachedAt = -1e9;
}  // namespace

void checkForUpdate(std::function<void(const UpdateCheck&)> done) {
  const double now = [NSDate timeIntervalSinceReferenceDate];
  {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (gCached.ok && now - gCachedAt < 3600.0) {
      const UpdateCheck cached = gCached;
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ done(cached); });
      return;
    }
  }
  NSString* url = [NSString stringWithFormat:@"https://api.github.com/repos/%s/releases/latest", kRepository];
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
  request.timeoutInterval = 10.0;
  [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
  [request setValue:[NSString stringWithFormat:@"midihands/%s", kVersion] forHTTPHeaderField:@"User-Agent"];
  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
          UpdateCheck result;
          const NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]]
                                       ? static_cast<NSHTTPURLResponse*>(response).statusCode : 0;
          if (error || !data) {
            result.error = "offline";
          } else if (status == 404) {
            result.ok = true;  // no release published yet
          } else if (status != 200) {
            result.error = "GitHub answered " + std::to_string(status);
          } else {
            id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            NSString* tag = [json isKindOfClass:[NSDictionary class]] ? json[@"tag_name"] : nil;
            if ([tag isKindOfClass:[NSString class]]) {
              result.ok = true;
              result.latest = tag.UTF8String;
              if (!result.latest.empty() && result.latest[0] == 'v') result.latest.erase(0, 1);
              result.newer = compareVersions(result.latest, kVersion) > 0;
            } else {
              result.error = "unexpected answer from GitHub";
            }
          }
          if (result.ok) {
            std::lock_guard<std::mutex> lock(gCacheMutex);
            gCached = result;
            gCachedAt = [NSDate timeIntervalSinceReferenceDate];
          }
          done(result);
        }];
  [task resume];
}

void installUpdate(std::function<void(bool, const std::string&)> done) {
  // curl leaves no quarantine flag, so macOS loads the new external without
  // a Developer ID signature, exactly as with a manual install.
  NSString* script = [NSString
      stringWithFormat:@"curl -fsSL https://github.com/%s/releases/latest/download/install.sh | /bin/bash -s -- --yes",
                       kRepository];
  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
  task.arguments = @[ @"-o", @"pipefail", @"-c", script ];
  NSPipe* output = [NSPipe pipe];
  task.standardOutput = output;
  task.standardError = output;
  NSMutableData* collected = [NSMutableData data];  // read as it comes, so a full pipe never blocks the script
  output.fileHandleForReading.readabilityHandler = ^(NSFileHandle* handle) {
    NSData* chunk = handle.availableData;
    @synchronized(collected) { [collected appendData:chunk]; }
  };
  task.terminationHandler = ^(NSTask* finished) {
    output.fileHandleForReading.readabilityHandler = nil;
    NSData* rest = [output.fileHandleForReading readDataToEndOfFile];
    NSString* text = nil;
    @synchronized(collected) {
      [collected appendData:rest];
      text = [[NSString alloc] initWithData:collected encoding:NSUTF8StringEncoding] ?: @"";
    }
    NSArray<NSString*>* lines =
        [[text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            componentsSeparatedByString:@"\n"];
    const std::string last = lines.count ? lines.lastObject.UTF8String : "";
    done(finished.terminationStatus == 0, last);
  };
  NSError* error = nil;
  if (![task launchAndReturnError:&error]) done(false, error.localizedDescription.UTF8String ?: "could not start the installer");
}

}  // namespace mh
