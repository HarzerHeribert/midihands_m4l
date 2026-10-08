#include "../platform/shell.hpp"

#import <AppKit/AppKit.h>

namespace mh {

void revealFile(const std::string& path) {
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ url ]];
}

}  // namespace mh
