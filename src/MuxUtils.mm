#include <juce_core/juce_core.h>
#if JUCE_MAC
#import <AVFoundation/AVFoundation.h>

static BOOL exportCompositionSync(AVMutableComposition* composition, NSURL* dstURL, AVFileType fileType, NSError** outError) {
    [[NSFileManager defaultManager] removeItemAtURL:dstURL error:nil];
    AVAssetExportSession* exporter = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetHighestQuality];
    exporter.outputURL = dstURL;
    exporter.outputFileType = fileType;
    __block BOOL finished = NO;
    __block NSError* exportError = nil;
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    [exporter exportAsynchronouslyWithCompletionHandler:^{
        if (exporter.status != AVAssetExportSessionStatusCompleted) exportError = exporter.error;
        finished = YES;
        dispatch_semaphore_signal(sema);
    }];
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    if (outError) *outError = exportError;
    return finished && exportError == nil;
}

bool juceMuxAudioVideo(const juce::File& audioWav, const juce::File& videoMov, const juce::File& outMov, juce::String& errorOut) {
    @autoreleasepool {
        if (!audioWav.existsAsFile() || !videoMov.existsAsFile()) { errorOut = "Missing source files"; return false; }
        AVURLAsset* audio = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:audioWav.getFullPathName().toRawUTF8()]] options:nil];
        AVURLAsset* video = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:videoMov.getFullPathName().toRawUTF8()]] options:nil];

        AVMutableComposition* comp = [AVMutableComposition composition];
        CMTime start = kCMTimeZero;

        // Add video track
        AVAssetTrack* vTrack = [[video tracksWithMediaType:AVMediaTypeVideo] firstObject];
        if (vTrack) {
            AVMutableCompositionTrack* compV = [comp addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
            if (![compV insertTimeRange:CMTimeRangeMake(kCMTimeZero, video.duration) ofTrack:vTrack atTime:start error:nil]) {
                errorOut = "Failed to insert video track"; return false;
            }
        }
        // Add audio track
        AVAssetTrack* aTrack = [[audio tracksWithMediaType:AVMediaTypeAudio] firstObject];
        if (aTrack) {
            AVMutableCompositionTrack* compA = [comp addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid];
            CMTime audioDur = audio.duration;
            CMTime videoDur = video.duration;
            CMTime dur = CMTIME_COMPARE_INLINE(audioDur, >, videoDur) ? videoDur : audioDur;
            if (![compA insertTimeRange:CMTimeRangeMake(kCMTimeZero, dur) ofTrack:aTrack atTime:start error:nil]) {
                errorOut = "Failed to insert audio track"; return false;
            }
        }

        // Choose output type by extension
        auto ext = outMov.getFileExtension().toLowerCase();
        AVFileType fileType = AVFileTypeQuickTimeMovie;
        if (ext == ".mp4") fileType = AVFileTypeMPEG4;

        NSError* err = nil;
        BOOL ok = exportCompositionSync(comp, [NSURL fileURLWithPath:[NSString stringWithUTF8String:outMov.getFullPathName().toRawUTF8()]], fileType, &err);
        if (!ok) { if (err) errorOut = juce::String([err localizedDescription].UTF8String); else errorOut = "Export failed"; }
        return ok;
    }
}
#endif
