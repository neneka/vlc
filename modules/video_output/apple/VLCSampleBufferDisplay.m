/*****************************************************************************
 * VLCSampleBufferDisplay.m: video output display using
 * AVSampleBufferDisplayLayer on all Apple platforms
 *****************************************************************************
 * Copyright (C) 2023-2026 VLC authors and VideoLAN
 *
 * Authors: Maxime Chapelet <umxprime at videolabs dot io>
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_atomic.h>
#include <vlc_modules.h>

#import "VLCDrawable.h"

#import <AVFoundation/AVFoundation.h>
#import <AVKit/AVKit.h>

#include "../../codec/vt_utils.h"
#include "vlc_pip_controller.h"

#import <VideoToolbox/VideoToolbox.h>

#if __is_target_os(ios)
#define IS_VT_ROTATION_API_AVAILABLE __IPHONE_OS_VERSION_MAX_ALLOWED >= 160000
#elif __is_target_os(macos)
#define IS_VT_ROTATION_API_AVAILABLE __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
#elif __is_target_os(tvos)
#define IS_VT_ROTATION_API_AVAILABLE __TV_OS_VERSION_MAX_ALLOWED >= 160000
#elif __is_target_os(visionos)
#define IS_VT_ROTATION_API_AVAILABLE __VISION_OS_VERSION_MAX_ALLOWED >= 10000
#endif

typedef NS_ENUM(NSUInteger, VLCSampleBufferPixelRotation) {
    kVLCSampleBufferPixelRotation_0 = 0,
    kVLCSampleBufferPixelRotation_90CW,
    kVLCSampleBufferPixelRotation_180,
    kVLCSampleBufferPixelRotation_90CCW,
};

typedef NS_ENUM(NSUInteger, VLCSampleBufferPixelFlip) {
    kVLCSampleBufferPixelFlip_None = 0,
    kVLCSampleBufferPixelFlip_H = 1 << 0,
    kVLCSampleBufferPixelFlip_V = 1 << 1,
};

#pragma mark - VLCRotatedPixelBufferProvider

@interface VLCRotatedPixelBufferProvider : NSObject
- (CVPixelBufferRef)provideFromBuffer:(CVPixelBufferRef)pixelBuffer
                             rotation:(VLCSampleBufferPixelRotation)rotation;
@end

@implementation VLCRotatedPixelBufferProvider
{
    CVPixelBufferPoolRef _rotationPool;
}

- (BOOL)_validateRotationPoolWithBuffer:(CVPixelBufferRef)pixelBuffer
                               rotation:(VLCSampleBufferPixelRotation)rotation
{
    if (!_rotationPool)
        return NO;

    uint32_t poolWidth, poolHeigth, bufferWidth, bufferHeight;

    bufferWidth = (uint32_t)CVPixelBufferGetWidth(pixelBuffer);
    bufferHeight = (uint32_t)CVPixelBufferGetHeight(pixelBuffer);
    if (rotation == kVLCSampleBufferPixelRotation_90CW || rotation == kVLCSampleBufferPixelRotation_90CCW)
    {
        uint32_t swap = bufferWidth;
        bufferWidth = bufferHeight;
        bufferHeight = swap;
    }

    CFDictionaryRef poolAttr = CVPixelBufferPoolGetPixelBufferAttributes(_rotationPool);
    if (!poolAttr) {
        return NO;
    }
    CFTypeRef value;
    value = CFDictionaryGetValue(poolAttr, kCVPixelBufferWidthKey);
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()
        || !CFNumberGetValue(value, kCFNumberIntType, &poolWidth)
        || poolWidth != bufferWidth)
    {
        return NO;
    }

    value = CFDictionaryGetValue(poolAttr, kCVPixelBufferHeightKey);
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()
        || !CFNumberGetValue(value, kCFNumberIntType, &poolHeigth)
        || poolHeigth != bufferHeight)
    {
        return NO;
    }

    return YES;
}

- (CVPixelBufferRef)provideFromBuffer:(CVPixelBufferRef)pixelBuffer
                             rotation:(VLCSampleBufferPixelRotation)rotation
{
    if (![self _validateRotationPoolWithBuffer:pixelBuffer rotation:rotation])
        CVPixelBufferPoolRelease(_rotationPool);

    if (!_rotationPool) {
        bool rotated = rotation == kVLCSampleBufferPixelRotation_90CW || rotation == kVLCSampleBufferPixelRotation_90CCW;
        uint32_t srcWidth = CVPixelBufferGetWidth(pixelBuffer);
        uint32_t srcHeight = CVPixelBufferGetHeight(pixelBuffer);
        uint32_t dstWidth = rotated ? srcHeight : srcWidth;
        uint32_t dstHeight = rotated ? srcWidth : srcHeight;

        CFTypeRef keys[] = {
            kCVPixelBufferPixelFormatTypeKey,
            kCVPixelBufferWidthKey,
            kCVPixelBufferHeightKey,
            kCVPixelBufferIOSurfacePropertiesKey,
            kCVPixelBufferMetalCompatibilityKey,
#if TARGET_OS_OSX
            kCVPixelBufferOpenGLCompatibilityKey,
#elif !defined(TARGET_OS_VISION) || !TARGET_OS_VISION
            kCVPixelBufferOpenGLESCompatibilityKey,
#endif
        };

        CFTypeRef values[] = {
            (__bridge CFNumberRef)(@(CVPixelBufferGetPixelFormatType(pixelBuffer))),
            (__bridge CFNumberRef)(@(dstWidth)),
            (__bridge CFNumberRef)(@(dstHeight)),
            (__bridge CFDictionaryRef)@{},
            kCFBooleanTrue,
#if !defined(TARGET_OS_VISION) || !TARGET_OS_VISION
            kCFBooleanTrue
#endif
        };
        _Static_assert(ARRAY_SIZE(keys) == ARRAY_SIZE(values),
            "Mismatch between keys and values array sizes");

        CFDictionaryRef poolAttr = CFDictionaryCreate(NULL, keys, values, ARRAY_SIZE(keys), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

        OSStatus status = CVPixelBufferPoolCreate(NULL, NULL, poolAttr, &_rotationPool);
        CFRelease(poolAttr);
        if (status != noErr)
            return NULL;
    }

    CVPixelBufferRef rotated;
    OSStatus status = CVPixelBufferPoolCreatePixelBuffer(NULL, _rotationPool, &rotated);
    if (status != noErr) {
        return NULL;
    }
    CFDictionaryRef attachments;
    if (@available(iOS 15.0, tvOS 15.0, macOS 12.0, *)) {
        attachments = CVBufferCopyAttachments(pixelBuffer, kCVAttachmentMode_ShouldPropagate);
    } else {
        attachments = CVBufferGetAttachments(pixelBuffer, kCVAttachmentMode_ShouldPropagate);
    }
    CVBufferSetAttachments(rotated, attachments, kCVAttachmentMode_ShouldPropagate);
    if (@available(iOS 15.0, tvOS 15.0, macOS 12.0, *)) {
        CFRelease(attachments);
    }
    return rotated;
}

- (void)dealloc
{
    CVPixelBufferPoolRelease(_rotationPool);
}
@end

#pragma mark - VLCPixelBufferRotationContext

@protocol VLCPixelBufferRotationContext
@property(nonatomic) VLCSampleBufferPixelRotation rotation;
@property(nonatomic) VLCSampleBufferPixelFlip flip;
- (CVPixelBufferRef)rotate:(CVPixelBufferRef)pixelBuffer;
@end

#pragma mark - VLCPixelBufferRotationContextVT

#if IS_VT_ROTATION_API_AVAILABLE
API_AVAILABLE(ios(16.0), tvos(16.0), macosx(13.0))
@interface VLCPixelBufferRotationContextVT : NSObject <VLCPixelBufferRotationContext>

@end

@implementation VLCPixelBufferRotationContextVT
{
    VLCRotatedPixelBufferProvider *_bufferProvider;
    VTPixelRotationSessionRef _rotationSession;
}

@synthesize rotation = _rotation, flip = _flip;

- (instancetype)init
{
    self = [super init];
    if (self) {
        if (@available(iOS 16.0, tvOS 16.0, macOS 13.0, *)) {
            OSStatus status = VTPixelRotationSessionCreate(NULL, &_rotationSession);
            if (status != noErr)
                return nil;
        } else {
            return nil;
        }
    }
    return self;
}

- (void)setRotation:(VLCSampleBufferPixelRotation)rotation {
    if (_rotation == rotation)
        return;
    _rotation = rotation;
    switch (rotation) {
        case kVLCSampleBufferPixelRotation_90CW:
            VTSessionSetProperty(_rotationSession, kVTPixelRotationPropertyKey_Rotation, kVTRotation_CW90);
            break;
        case kVLCSampleBufferPixelRotation_180:
            VTSessionSetProperty(_rotationSession, kVTPixelRotationPropertyKey_Rotation, kVTRotation_180);
            break;
        case kVLCSampleBufferPixelRotation_90CCW:
            VTSessionSetProperty(_rotationSession, kVTPixelRotationPropertyKey_Rotation, kVTRotation_CCW90);
            break;
        case kVLCSampleBufferPixelRotation_0:
        default:
            VTSessionSetProperty(_rotationSession, kVTPixelRotationPropertyKey_Rotation, kVTRotation_0);
            break;
    }
}

- (void)setFlip:(VLCSampleBufferPixelFlip)flip {
    if (_flip == flip)
        return;
    _flip = flip;
    VTSessionSetProperty(_rotationSession, kVTPixelRotationPropertyKey_FlipHorizontalOrientation, flip & kVLCSampleBufferPixelFlip_H ? kCFBooleanTrue : kCFBooleanFalse);
    VTSessionSetProperty(_rotationSession, kVTPixelRotationPropertyKey_FlipVerticalOrientation, flip & kVLCSampleBufferPixelFlip_V ? kCFBooleanTrue : kCFBooleanFalse);
}

- (CVPixelBufferRef)rotate:(CVPixelBufferRef)pixelBuffer {
    if (!_bufferProvider)
        _bufferProvider = [VLCRotatedPixelBufferProvider new];

    CVPixelBufferRef rotated;
    rotated = [_bufferProvider provideFromBuffer:pixelBuffer rotation:_rotation];
    if (!rotated)
        return NULL;

    OSStatus status = VTPixelRotationSessionRotateImage(_rotationSession, pixelBuffer, rotated);
    if (status != noErr) {
        CFRelease(rotated);
        return NULL;
    }

    return rotated;
}

- (void)dealloc
{
    if (_rotationSession) {
        VTPixelRotationSessionInvalidate(_rotationSession);
        CFRelease(_rotationSession);
    }
}

@end

#endif // IS_VT_ROTATION_API_AVAILABLE

#pragma mark - VLCPixelBufferRotationContextCI

@interface VLCPixelBufferRotationContextCI : NSObject <VLCPixelBufferRotationContext>

@end

@implementation VLCPixelBufferRotationContextCI
{
    VLCRotatedPixelBufferProvider *_bufferProvider;
    CIContext *_rotationContext;
    CGImagePropertyOrientation _orientation;
}

@synthesize rotation = _rotation, flip = _flip;

- (instancetype)init
{
    self = [super init];
    if (self) {
        _rotationContext = [[CIContext alloc] initWithOptions:nil];
        if (!_rotationContext)
            return nil;
    }
    return self;
}

- (void)_updateOrientation {
    switch (_rotation) {
        case kVLCSampleBufferPixelRotation_90CW:
        {
            if (_flip == kVLCSampleBufferPixelFlip_None)
                _orientation = kCGImagePropertyOrientationRight;
            if (_flip == kVLCSampleBufferPixelFlip_H)
                _orientation = kCGImagePropertyOrientationLeftMirrored;
            if (_flip == kVLCSampleBufferPixelFlip_V)
                _orientation = kCGImagePropertyOrientationRightMirrored;
            if (_flip == (kVLCSampleBufferPixelFlip_H | kVLCSampleBufferPixelFlip_V))
                _orientation = kCGImagePropertyOrientationLeft;
            break;
        }
        case kVLCSampleBufferPixelRotation_180:
        {
            if (_flip == kVLCSampleBufferPixelFlip_None)
                _orientation = kCGImagePropertyOrientationDown;
            if (_flip == kVLCSampleBufferPixelFlip_H)
                _orientation = kCGImagePropertyOrientationDownMirrored;
            if (_flip == kVLCSampleBufferPixelFlip_V)
                _orientation = kCGImagePropertyOrientationUpMirrored;
            if (_flip == (kVLCSampleBufferPixelFlip_H | kVLCSampleBufferPixelFlip_V))
                _orientation = kCGImagePropertyOrientationUp;
            break;
        }
        case kVLCSampleBufferPixelRotation_90CCW:
        {
            if (_flip == kVLCSampleBufferPixelFlip_None)
                _orientation = kCGImagePropertyOrientationLeft;
            if (_flip == kVLCSampleBufferPixelFlip_H)
                _orientation = kCGImagePropertyOrientationRightMirrored;
            if (_flip == kVLCSampleBufferPixelFlip_V)
                _orientation = kCGImagePropertyOrientationLeftMirrored;
            if (_flip == (kVLCSampleBufferPixelFlip_H | kVLCSampleBufferPixelFlip_V))
                _orientation = kCGImagePropertyOrientationRight;
            break;
        }
        case kVLCSampleBufferPixelRotation_0:
        default:
        {
            if (_flip == kVLCSampleBufferPixelFlip_None)
                _orientation = kCGImagePropertyOrientationUp;
            if (_flip == kVLCSampleBufferPixelFlip_H)
                _orientation = kCGImagePropertyOrientationUpMirrored;
            if (_flip == kVLCSampleBufferPixelFlip_V)
                _orientation = kCGImagePropertyOrientationDownMirrored;
            if (_flip == (kVLCSampleBufferPixelFlip_H | kVLCSampleBufferPixelFlip_V))
                _orientation = kCGImagePropertyOrientationDown;
            break;
        }
    }
}

- (void)setRotation:(VLCSampleBufferPixelRotation)rotation {
    if (_rotation == rotation)
        return;
    _rotation = rotation;
    [self _updateOrientation];
}

- (void)setFlip:(VLCSampleBufferPixelFlip)flip {
    if (_flip == flip)
        return;
    _flip = flip;
    [self _updateOrientation];
}

- (CVPixelBufferRef)rotate:(CVPixelBufferRef)pixelBuffer {
    if (!_bufferProvider)
        _bufferProvider = [VLCRotatedPixelBufferProvider new];

    CVPixelBufferRef rotated;
    rotated = [_bufferProvider provideFromBuffer:pixelBuffer rotation:_rotation];
    if (!rotated)
        return NULL;

    CIImage *image = [[CIImage alloc] initWithCVPixelBuffer:pixelBuffer];
    image = [image imageByApplyingOrientation:_orientation];
    [_rotationContext render:image toCVPixelBuffer:rotated];

    return rotated;
}

@end

#pragma mark - VLCHDRToSDRConverter

@interface VLCHDRToSDRConverter : NSObject
- (CVPixelBufferRef)copyPixelBufferByToneMapping:(CVPixelBufferRef)pixelBuffer
                                      useHLGCurve:(BOOL)useHLGCurve
                               bufferLimitReached:(BOOL *)bufferLimitReached
    CF_RETURNS_RETAINED;
@end

/* Six 4K BGRA buffers occupy about 190 MiB. This is enough for rendering
 * latency without allowing AVSampleBufferDisplayLayer to retain an unbounded
 * number of converted frames. */
static const unsigned VLCHDRToSDRMaxBuffers = 6;

static NSString * const VLCHLGToSDRKernelSource = @
    "kernel vec4 hlgToSDR(__sample pixel) {"
    "    vec3 rgb = max(pixel.rgb, vec3(0.0));"
    "    float luminance = max(dot(rgb, vec3(0.2126, 0.7152, 0.0722)), 1.0e-6);"
    "    const float knee = 0.80;"
    "    const float range = 1.0 - knee;"
    "    float mappedLuminance = luminance;"
    "    if (luminance > knee) {"
    "        float highlight = luminance - knee;"
    "        mappedLuminance = knee + highlight / (1.0 + highlight / range);"
    "    }"
    "    vec3 mapped = rgb * (mappedLuminance / luminance);"
    "    return vec4(clamp(mapped, 0.0, 1.0), pixel.a);"
    "}";

@implementation VLCHDRToSDRConverter
{
    CIContext *_context;
    CIColorKernel *_hlgToSDRKernel;
    NSDictionary *_poolAuxAttributes;
    CVPixelBufferPoolRef _pool;
    CGColorSpaceRef _outputColorSpace;
    size_t _width;
    size_t _height;
}

- (instancetype)init
{
    self = [super init];
    if (!self)
        return nil;

    _poolAuxAttributes = @{
        (__bridge NSString *)kCVPixelBufferPoolAllocationThresholdKey:
            @(VLCHDRToSDRMaxBuffers),
    };

    if (@available(macOS 11.0, iOS 14.1, tvOS 14.0, *)) {
        CGColorSpaceRef workingColorSpace =
            CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
        if (!workingColorSpace)
            return nil;
        _context = [CIContext contextWithOptions:@{
            kCIContextCacheIntermediates: @NO,
            kCIContextWorkingColorSpace: (__bridge id)workingColorSpace,
        }];
        CGColorSpaceRelease(workingColorSpace);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        _hlgToSDRKernel = [CIColorKernel kernelWithString:VLCHLGToSDRKernelSource];
#pragma clang diagnostic pop

        _outputColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
        if (!_context || !_hlgToSDRKernel || !_outputColorSpace)
            return nil;
    } else {
        return nil;
    }

    return self;
}

- (BOOL)preparePoolForPixelBuffer:(CVPixelBufferRef)pixelBuffer
{
    const size_t width = CVPixelBufferGetWidth(pixelBuffer);
    const size_t height = CVPixelBufferGetHeight(pixelBuffer);
    if (_pool && width == _width && height == _height)
        return YES;

    if (_pool) {
        CVPixelBufferPoolRelease(_pool);
        _pool = NULL;
    }

    NSDictionary *attributes = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_32BGRA),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(width),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(height),
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    OSStatus status = CVPixelBufferPoolCreate(
        NULL, NULL, (__bridge CFDictionaryRef)attributes, &_pool);
    if (status != noErr)
        return NO;

    _width = width;
    _height = height;
    return YES;
}

static void CopyGeometryAttachment(CVPixelBufferRef source,
                                   CVPixelBufferRef destination,
                                   CFStringRef key)
{
    CFTypeRef value;
    CVAttachmentMode mode = kCVAttachmentMode_ShouldPropagate;
    if (@available(macOS 12.0, iOS 15.0, tvOS 15.0, *)) {
        value = CVBufferCopyAttachment(source, key, &mode);
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        value = CVBufferGetAttachment(source, key, &mode);
        if (value)
            CFRetain(value);
#pragma clang diagnostic pop
    }

    if (value) {
        CVBufferSetAttachment(destination, key, value, mode);
        CFRelease(value);
    }
}

- (CVPixelBufferRef)copyPixelBufferByToneMapping:(CVPixelBufferRef)pixelBuffer
                                      useHLGCurve:(BOOL)useHLGCurve
                               bufferLimitReached:(BOOL *)bufferLimitReached
{
    if (bufferLimitReached)
        *bufferLimitReached = NO;

    if (![self preparePoolForPixelBuffer:pixelBuffer])
        return NULL;

    CVPixelBufferRef output = NULL;
    CVReturn status = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
        NULL, _pool, (__bridge CFDictionaryRef)_poolAuxAttributes, &output);
    if (status != kCVReturnSuccess) {
        if (bufferLimitReached &&
            status == kCVReturnWouldExceedAllocationThreshold)
            *bufferLimitReached = YES;
        return NULL;
    }

    CFStringRef outputTransferFunction =
        kCVImageBufferTransferFunction_ITU_R_709_2;
    if (@available(macOS 11.0, iOS 14.1, tvOS 14.0, *)) {
        CIImage *image = [[CIImage alloc] initWithCVPixelBuffer:pixelBuffer
            options:@{ kCIImageToneMapHDRtoSDR: @(!useHLGCurve) }];
        CGColorSpaceRef renderColorSpace = _outputColorSpace;

        if (useHLGCurve) {
            /* Core Image has already decoded HLG to its extended-linear
             * working space. Preserve those midtones, compress only values
             * near SDR peak white, then explicitly encode the pixels. Passing
             * no render color space prevents a second, scene-adaptive system
             * tone map from undoing this fixed curve. */
            image = [_hlgToSDRKernel applyWithExtent:image.extent
                                           arguments:@[image]];
            image = [image imageByApplyingFilter:@"CILinearToSRGBToneCurve"];
            renderColorSpace = NULL;
            outputTransferFunction = kCVImageBufferTransferFunction_sRGB;
        }

        if (!image) {
            CVPixelBufferRelease(output);
            return NULL;
        }

        CGRect bounds = CGRectMake(0, 0, _width, _height);
        [_context render:image
          toCVPixelBuffer:output
                  bounds:bounds
              colorSpace:renderColorSpace];
    }

    /* Pool buffers can be reused. Clear any stale metadata, retain only the
     * source geometry, and describe the converted pixels as SDR Rec. 709. */
    CVBufferRemoveAllAttachments(output);
    CopyGeometryAttachment(pixelBuffer, output,
                           kCVImageBufferCleanApertureKey);
    CopyGeometryAttachment(pixelBuffer, output,
                           kCVImageBufferPixelAspectRatioKey);
    CVBufferSetAttachment(output, kCVImageBufferColorPrimariesKey,
                          kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(output, kCVImageBufferTransferFunctionKey,
                          outputTransferFunction,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(output, kCVImageBufferYCbCrMatrixKey,
                          kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    return output;
}

- (void)dealloc
{
    if (_pool)
        CVPixelBufferPoolRelease(_pool);
    if (_outputColorSpace)
        CGColorSpaceRelease(_outputColorSpace);
}

@end

static vlc_decoder_device * CVPXHoldDecoderDevice(vlc_object_t *o, void *sys)
{
    VLC_UNUSED(o);
    vout_display_t *vd = sys;
    vlc_decoder_device *device =
        vlc_decoder_device_Create(VLC_OBJECT(vd), vd->cfg->window);
    static const struct vlc_decoder_device_operations ops =
    {
        NULL,
    };
    device->ops = &ops;
    device->type = VLC_DECODER_DEVICE_VIDEOTOOLBOX;
    return device;
}

static filter_t *
CreateCVPXConverter(vout_display_t *vd, const video_format_t *fmt)
{
    filter_t *converter = vlc_object_create(vd, sizeof(filter_t));
    if (!converter)
        return NULL;

    static const struct filter_video_callbacks cbs =
    {
        .buffer_new = NULL,
        .hold_device = CVPXHoldDecoderDevice,
    };
    converter->owner.video = &cbs;
    converter->owner.sys = vd;

    es_format_InitFromVideo(&converter->fmt_in, fmt);
    es_format_InitFromVideo(&converter->fmt_out, fmt);

    converter->fmt_out.video.i_chroma =
    converter->fmt_out.i_codec = VLC_CODEC_CVPX_BGRA;

    converter->p_module = vlc_filter_LoadModule(converter, "video converter", NULL, false);
    if (!converter->p_module)
    {
        vlc_object_delete(converter);
        return NULL;
    }
    assert( converter->ops != NULL );

    return converter;
}


static void DeleteCVPXConverter( filter_t * p_converter )
{
    if (!p_converter)
        return;

    vlc_filter_UnloadModule( p_converter );

    es_format_Clean( &p_converter->fmt_in );
    es_format_Clean( &p_converter->fmt_out );

    vlc_object_delete(p_converter);
}

static pip_controller_t * CreatePipController( vout_display_t *vd, void *cbs_opaque,
                                               void (*state_changed_cb)(void *, bool) );
static void DeletePipController( pip_controller_t * pipcontroller );
static void PictureInPictureStateChanged(void *opaque, bool is_started);

static const vlc_fourcc_t sample_buffer_display_subfmts[] = {
    VLC_CODEC_ARGB,
    0
};

#pragma mark -
@class VLCSampleBufferSubpicture, VLCSampleBufferDisplay;

@interface VLCSampleBufferSubpictureRegion: NSObject
@property (nonatomic, weak) VLCSampleBufferSubpicture *subpicture;
@property (nonatomic) CGRect backingFrame;
@property (nonatomic) CGImageRef image;
@property (nonatomic) CGFloat    alpha;
@end

@implementation VLCSampleBufferSubpictureRegion
- (void)dealloc {
    CGImageRelease(_image);
}
@end

#pragma mark -

@interface VLCSampleBufferSubpicture: NSObject
@property (nonatomic, weak) VLCSampleBufferDisplay *sys;
@property (nonatomic) NSArray<VLCSampleBufferSubpictureRegion *> *regions;
@property (nonatomic) int64_t order;
@end

@implementation VLCSampleBufferSubpicture

@end

#pragma mark -

@interface VLCSampleBufferSubpictureView: VLCView
- (void)drawSubpicture:(VLCSampleBufferSubpicture *)subpicture;
@end

@implementation VLCSampleBufferSubpictureView
{
    VLCSampleBufferSubpicture *_pendingSubpicture;
}

- (instancetype)init {
    self = [super init];
    if (!self)
        return nil;
#if TARGET_OS_OSX
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.wantsLayer = YES;
#else
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundColor = [UIColor clearColor];
#endif
    return self;
}

- (void)drawSubpicture:(VLCSampleBufferSubpicture *)subpicture {
    _pendingSubpicture = subpicture;
#if TARGET_OS_OSX
    [self setNeedsDisplay:YES];
#else
    [self setNeedsDisplay];
#endif
}

- (void)drawRect:(CGRect)dirtyRect {
    #if TARGET_OS_OSX
    NSGraphicsContext *graphicsCtx = [NSGraphicsContext currentContext];
    CGContextRef cgCtx = [graphicsCtx CGContext];
    #else
    CGContextRef cgCtx = UIGraphicsGetCurrentContext();
    #endif

    CGContextClearRect(cgCtx, self.bounds);

    /* Capture a strong local reference to the pending subpicture so that
     * a concurrent update from the VLC rendering thread cannot swap it
     * (and release the underlying CGImageRef) while we are iterating
     * over the regions. Without this, a window resize can cause
     * EXC_BAD_ACCESS (code=1) in CGContextDrawImage. */
    VLCSampleBufferSubpicture *subpicture = _pendingSubpicture;
    NSArray<VLCSampleBufferSubpictureRegion *> *regions = subpicture.regions;

#if TARGET_OS_IPHONE
    CGContextSaveGState(cgCtx);
    CGAffineTransform translate = CGAffineTransformTranslate(CGAffineTransformIdentity, 0.0, self.frame.size.height);
    CGFloat scale = 1.0f / self.contentScaleFactor;
    CGAffineTransform transform = CGAffineTransformScale(translate, scale, -scale);
    CGContextConcatCTM(cgCtx, transform);
#endif
    for (VLCSampleBufferSubpictureRegion *region in regions) {
        CGImageRef image = region.image;
        if (!image)
            continue;
#if TARGET_OS_OSX
        CGRect regionFrame = [self convertRectFromBacking:region.backingFrame];
#else
        CGRect regionFrame = region.backingFrame;
#endif
        CGContextSetAlpha(cgCtx, region.alpha);
        CGContextDrawImage(cgCtx, regionFrame, image);
    }
#if TARGET_OS_IPHONE
    CGContextRestoreGState(cgCtx);
#endif
}

@end

#pragma mark -

@interface VLCSampleBufferDisplayView: VLCView <CALayerDelegate>
- (AVSampleBufferDisplayLayer *)displayLayer;
@end

@implementation VLCSampleBufferDisplayView

- (instancetype)init {
    self = [super init];
    if (!self)
        return nil;
#if TARGET_OS_OSX
    self.autoresizingMask = NSViewNotSizable;
    self.wantsLayer = YES;
#else
    self.autoresizingMask = UIViewAutoresizingNone;
#endif
    return self;
}

#if TARGET_OS_OSX
- (CALayer *)makeBackingLayer {
    AVSampleBufferDisplayLayer *layer;
    layer = [AVSampleBufferDisplayLayer new];
    layer.delegate = self;
    layer.videoGravity = AVLayerVideoGravityResize;
    [CATransaction lock];
    layer.needsDisplayOnBoundsChange = YES;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    layer.opaque = 1.0;
    layer.hidden = NO;
    [CATransaction unlock];
    return layer;
}
#else
+ (Class)layerClass {
    return [AVSampleBufferDisplayLayer class];
}
#endif

- (AVSampleBufferDisplayLayer *)displayLayer {
    return (AVSampleBufferDisplayLayer *)self.layer;
}

#if TARGET_OS_OSX
/* Layer delegate method that ensures the layer always get the
 * correct contentScale based on whether the view is on a HiDPI
 * display or not, and when it is moved between displays.
 */
- (BOOL)layer:(CALayer *)layer
shouldInheritContentsScale:(CGFloat)newScale
   fromWindow:(NSWindow *)window
{
    return YES;
}
#endif

/*
 * General properties
 */

- (BOOL)isOpaque
{
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

@end

#pragma mark -

@interface VLCSampleBufferDisplay: NSObject
{
    @public
    filter_t *converter;
}
    @property (nonatomic, readonly, weak) VLCView *window;
    @property (nonatomic, readonly) vout_display_t *vd;
    @property (nonatomic) VLCSampleBufferDisplayView *displayView;
    @property (nonatomic) AVSampleBufferDisplayLayer *displayLayer;
    @property (nonatomic) VLCSampleBufferSubpictureView *spuView;
    @property (nonatomic) VLCSampleBufferSubpicture *subpicture;
    @property (nonatomic) id<VLCPixelBufferRotationContext> rotationContext;
    @property (nonatomic) VLCHDRToSDRConverter *hdrToSDRConverter;
    @property (atomic) BOOL pictureInPictureStarted;
    @property (atomic) BOOL hdrToneMappingFailed;
#if TARGET_OS_OSX
    @property (atomic) BOOL displaySupportsEDR;
#endif

    @property (nonatomic, readonly) pip_controller_t *pipcontroller;

    - (instancetype)init NS_UNAVAILABLE;
    + (instancetype)new NS_UNAVAILABLE;
    - (instancetype)initWithVoutDisplay:(vout_display_t *)vd;
    - (void)placeVideo:(vout_display_place_t)newPlace;
    - (void)handlePictureInPictureStateChange:(BOOL)isStarted;
#if TARGET_OS_OSX
    - (void)startObservingDisplayDynamicRange;
    - (void)stopObservingDisplayDynamicRange;
#endif
@end

@implementation VLCSampleBufferDisplay

#if TARGET_OS_OSX
- (void)updateDisplayDynamicRange:(NSNotification *)notification
{
    VLC_UNUSED(notification);
    NSScreen *screen = self.displayView.window.screen;
    BOOL supportsEDR = NO;
    if (@available(macOS 10.15, *))
        supportsEDR =
            screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;

    if (self.displaySupportsEDR == supportsEDR)
        return;

    self.displaySupportsEDR = supportsEDR;

    /* Discard frames prepared for the previous display mode. The next frame
     * will carry either the original HDR signal or the mapped SDR signal. */
    @synchronized(self.displayLayer) {
        if (self.displayLayer) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [self.displayLayer flush];
#pragma clang diagnostic pop
        }
    }
}

- (void)startObservingDisplayDynamicRange
{
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self
               selector:@selector(updateDisplayDynamicRange:)
                   name:NSWindowDidChangeScreenNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(updateDisplayDynamicRange:)
                   name:NSApplicationDidChangeScreenParametersNotification
                 object:nil];
    [self updateDisplayDynamicRange:nil];
}

- (void)stopObservingDisplayDynamicRange
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
#endif

- (id<VLCPixelBufferRotationContext>)rotationContext
{
    if (_rotationContext)
        return _rotationContext;
#if IS_VT_ROTATION_API_AVAILABLE
    if (@available(iOS 16.0, tvOS 16.0, macOS 13.0, *))
        _rotationContext = [VLCPixelBufferRotationContextVT new];
#endif
    if (!_rotationContext)
        _rotationContext = [VLCPixelBufferRotationContextCI new];
    return _rotationContext;
}


- (instancetype)initWithVoutDisplay:(vout_display_t *)vd
{
    self = [super init];
    if (!self)
        return nil;

    if (vd->cfg->window->type != VLC_WINDOW_TYPE_NSOBJECT)
        return nil;

    VLCView *window = (__bridge VLCView *)vd->cfg->window->handle.nsobject;
    if (!window) {
        msg_Err(vd, "No window found!");
        return nil;
    }

    _window = window;

    _pipcontroller = CreatePipController(vd, (__bridge void *)self,
                                         PictureInPictureStateChanged);

    _vd = vd;

    if (var_InheritBool(vd, "samplebuffer-stable-hdr-tone-mapping"))
        _hdrToSDRConverter = [VLCHDRToSDRConverter new];

    return self;
}

- (void)preparePictureInPicture {
    if ( !_pipcontroller)
        return;

    if ( _pipcontroller->ops->set_display_layer ) {
        _pipcontroller->ops->set_display_layer(
            _pipcontroller,
            (__bridge void*)_displayView.displayLayer
        );
    }
}

- (CGRect)frameForPlace:(const vout_display_place_t *)place
{
    VLCView *window = self.window;
    CGRect frame = CGRectMake(place->x, place->y, place->width, place->height);
#if TARGET_OS_OSX
    frame = [window convertRectFromBacking:frame];
    frame.origin.y = window.bounds.size.height - frame.origin.y - frame.size.height;
#else
    CGFloat scale = window.contentScaleFactor;
    frame.origin.x /= scale;
    frame.origin.y /= scale;
    frame.size.width /= scale;
    frame.size.height /= scale;
#endif
    return frame;
}

- (void)prepareDisplay {
    @synchronized(_displayLayer) {
        if (_displayLayer)
            return;
    }

    VLCSampleBufferDisplay *sys = self;
    vout_display_place_t place = *_vd->place;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (sys.displayView)
            return;

        VLCSampleBufferDisplayView *displayView;
        VLCSampleBufferSubpictureView *spuView;
        VLCView *window = sys.window;

        displayView = [[VLCSampleBufferDisplayView alloc] init];
        spuView = [VLCSampleBufferSubpictureView new];
        [window addSubview:displayView];
        [window addSubview:spuView];

        displayView.frame = [sys frameForPlace:&place];
        [spuView setFrame:[window bounds]];

        sys.displayView = displayView;
        sys.spuView = spuView;
#if TARGET_OS_OSX
        [sys startObservingDisplayDynamicRange];
#endif
        @synchronized(sys.displayLayer) {
            sys.displayLayer = displayView.displayLayer;
        }
        [sys preparePictureInPicture];
    });
}

- (void)placeVideo:(vout_display_place_t)newPlace {
    self.displayView.frame = [self frameForPlace:&newPlace];
}

- (void)close {
    VLCSampleBufferDisplay *sys = self;
    dispatch_async(dispatch_get_main_queue(), ^{
#if TARGET_OS_OSX
        [sys stopObservingDisplayDynamicRange];
#endif
        [sys.displayView removeFromSuperview];
        [sys.spuView removeFromSuperview];
    });
    DeletePipController(_pipcontroller);
    _pipcontroller = NULL;
}

- (void)handlePictureInPictureStateChange:(BOOL)isStarted
{
    self.pictureInPictureStarted = isStarted;
    if (isStarted)
        self.vd->info.subpicture_chromas = NULL;
    else
        self.vd->info.subpicture_chromas = sample_buffer_display_subfmts;

    /* Force the on-screen SPU view to clear stale regions immediately. */
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.spuView drawSubpicture:nil];
        self.spuView.hidden = isStarted;
    });
}

@end

#pragma mark -
#pragma mark Module functions

static void Close(vout_display_t *vd)
{
    VLCSampleBufferDisplay *sys;
    sys = (__bridge_transfer VLCSampleBufferDisplay*)vd->sys;

    DeleteCVPXConverter(sys->converter);

    [sys close];
}

static bool NeedsStableHDRToneMapping(const video_format_t *format)
{
    const bool is_hdr =
        format->transfer == TRANSFER_FUNC_SMPTE_ST2084 ||
        format->transfer == TRANSFER_FUNC_HLG;

    /* Preserve Dolby Vision's authored per-frame mapping. The stable fallback
     * is intended for HDR10/HLG sources without authored dynamic metadata. */
    return is_hdr && !format->dovi.rpu_present;
}

static bool ShouldApplyStableHDRToneMapping(VLCSampleBufferDisplay *sys,
                                            const video_format_t *format)
{
    if (!NeedsStableHDRToneMapping(format))
        return false;
#if TARGET_OS_OSX
    /* AVSampleBufferDisplayLayer can present the original 10-bit HDR signal
     * on an EDR-capable screen. Apply the fixed SDR mapping only when the
     * window is currently on an SDR screen. */
    return !sys.displaySupportsEDR;
#else
    return true;
#endif
}

static bool DisplayLayerCanAcceptMoreMediaData(
    AVSampleBufferDisplayLayer *displayLayer)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return displayLayer.status == AVQueuedSampleBufferRenderingStatusFailed ||
           displayLayer.readyForMoreMediaData;
#pragma clang diagnostic pop
}

static void RenderPicture(vout_display_t *vd, picture_t *pic, vlc_tick_t date) {
    VLCSampleBufferDisplay *sys;
    sys = (__bridge VLCSampleBufferDisplay*)vd->sys;

    @synchronized(sys.displayLayer) {
        if (sys.displayLayer == nil)
            return;

        /* Tone mapping detaches frames from the decoder's bounded surface
         * pool. Do not create another full-resolution buffer while the
         * display queue is already full. A failed layer is allowed through so
         * the existing flush-and-retry path can recover it below. */
        if (sys.hdrToSDRConverter &&
            ShouldApplyStableHDRToneMapping(sys, vd->fmt) &&
            !DisplayLayerCanAcceptMoreMediaData(sys.displayLayer))
            return;
    }

    switch (vd->fmt->orientation) {
    case ORIENT_HFLIPPED:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_H;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_0;
        break;
    case ORIENT_VFLIPPED:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_V;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_0;
        break;
    case ORIENT_ROTATED_90:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_None;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_90CW;
        break;
    case ORIENT_ROTATED_180:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_None;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_180;
        break;
    case ORIENT_ROTATED_270:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_None;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_90CCW;
        break;
    case ORIENT_TRANSPOSED:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_V;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_90CW;
        break;
    case ORIENT_ANTI_TRANSPOSED:
        sys.rotationContext.flip = kVLCSampleBufferPixelFlip_H;
        sys.rotationContext.rotation = kVLCSampleBufferPixelRotation_90CW;
    case ORIENT_NORMAL:
    default:
        sys.rotationContext = nil;
        break;
    }

    picture_Hold(pic);

    picture_t *dst = pic;
    if (sys->converter) {
        dst = sys->converter->ops->filter_video(sys->converter, pic);
    }

    BOOL dropFrame = NO;
    CVPixelBufferRef pixelBuffer = cvpxpic_get_ref(dst);
    if (pixelBuffer != NULL) {
        CVPixelBufferRetain(pixelBuffer);

        /* Both Core Image and CMVideoFormatDescriptionCreateForImageBuffer()
         * consume the color properties attached to the pixel buffer. */
        cvpx_attach_mapped_color_properties(pixelBuffer, &dst->format);

        if (sys.hdrToSDRConverter &&
            ShouldApplyStableHDRToneMapping(sys, &dst->format))
        {
            CVPixelBufferRef converted;
            @autoreleasepool {
                converted = [sys.hdrToSDRConverter
                    copyPixelBufferByToneMapping:pixelBuffer
                                      useHLGCurve:dst->format.transfer == TRANSFER_FUNC_HLG
                               bufferLimitReached:&dropFrame];
            }
            if (converted) {
                CVPixelBufferRelease(pixelBuffer);
                pixelBuffer = converted;
            } else if (dropFrame) {
                CVPixelBufferRelease(pixelBuffer);
                pixelBuffer = NULL;
            } else if (!sys.hdrToneMappingFailed) {
                msg_Warn(vd, "stable HDR-to-SDR tone mapping failed; using system conversion");
                sys.hdrToneMappingFailed = YES;
            }
        }
    }
    picture_Release(dst);

    if (pixelBuffer == NULL) {
        if (!dropFrame)
            msg_Err(vd, "No pixelBuffer ref attached to pic!");
        return;
    }

    if (vd->fmt->orientation != ORIENT_NORMAL) {
        CVPixelBufferRef rotated = [sys.rotationContext rotate:pixelBuffer];
        if (rotated) {
            CVPixelBufferRelease(pixelBuffer);
            pixelBuffer = rotated;
        }
    }

    unsigned aspectRatioNum = vd->source->i_sar_num;
    unsigned aspectRatioDen = vd->source->i_sar_den;
#if TARGET_OS_OSX
    /* VideoToolbox can return an aligned pixel buffer whose dimensions differ
     * from the visible video. Preserve the visible display aspect ratio for
     * AVSampleBufferDisplayLayer's private content layer. */
    video_format_t source;
    video_format_ApplyRotation(&source, vd->source);
    uint64_t aspectRatioScaledNum =
        (uint64_t)source.i_visible_width * source.i_sar_num *
        CVPixelBufferGetHeight(pixelBuffer);
    uint64_t aspectRatioScaledDen =
        (uint64_t)source.i_visible_height * source.i_sar_den *
        CVPixelBufferGetWidth(pixelBuffer);
    vlc_ureduce(&aspectRatioNum, &aspectRatioDen,
                aspectRatioScaledNum, aspectRatioScaledDen, 50000);
#endif

    id aspectRatio = @{
        (__bridge NSString*)kCVImageBufferPixelAspectRatioHorizontalSpacingKey:
            @(aspectRatioNum),
        (__bridge NSString*)kCVImageBufferPixelAspectRatioVerticalSpacingKey:
            @(aspectRatioDen)
    };

    CVBufferSetAttachment(
        pixelBuffer,
        kCVImageBufferPixelAspectRatioKey,
        (__bridge CFDictionaryRef)aspectRatio,
        kCVAttachmentMode_ShouldPropagate
    );

    CMSampleBufferRef sampleBuffer = NULL;
    CMVideoFormatDescriptionRef formatDesc = NULL;
    OSStatus err = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixelBuffer, &formatDesc);
    if (err != noErr) {
        msg_Err(vd, "Image buffer format desciption creation failed!");
        CVPixelBufferRelease(pixelBuffer);
        return;
    }

    vlc_tick_t now = vlc_tick_now();
    CFTimeInterval ca_now = CACurrentMediaTime();
    vlc_tick_t ca_now_ts = vlc_tick_from_sec(ca_now);
    vlc_tick_t diff = date - now;
    CFTimeInterval ca_date = secf_from_vlc_tick(ca_now_ts + diff);
    CMSampleTimingInfo sampleTimingInfo = {
        .decodeTimeStamp = kCMTimeInvalid,
        .duration = kCMTimeInvalid,
        .presentationTimeStamp = CMTimeMakeWithSeconds(ca_date, 1000000)
    };

    err = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixelBuffer, formatDesc, &sampleTimingInfo, &sampleBuffer);
    CFRelease(formatDesc);
    CVPixelBufferRelease(pixelBuffer);
    if (err != noErr) {
        msg_Err(vd, "Image buffer creation failed!");
        return;
    }

    @synchronized(sys.displayLayer) {
        if (sys.displayLayer.status == AVQueuedSampleBufferRenderingStatusFailed) {
            [sys.displayLayer flushAndRemoveImage];
        }
        [sys.displayLayer enqueueSampleBuffer:sampleBuffer];
    }

    CFRelease(sampleBuffer);
}

static CGRect RegionBackingFrame(unsigned display_height,
                                 const struct subpicture_region_rendered *r)
{
    // Invert y coords for CoreGraphics
    const int y = display_height - r->place.height - r->place.y;

    return CGRectMake(
        r->place.x,
        y,
        r->place.width,
        r->place.height
    );
}

static void UpdateSubpictureRegions(vout_display_t *vd,
                                    const vlc_render_subpicture *subpicture)
{
    VLCSampleBufferDisplay *sys;
    sys = (__bridge VLCSampleBufferDisplay*)vd->sys;

    if (sys.subpicture == nil || subpicture == NULL)
        return;

    NSMutableArray *regions = [NSMutableArray new];
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    const struct subpicture_region_rendered *r;
    vlc_vector_foreach(r, &subpicture->regions) {
        CFIndex length = r->p_picture->format.i_height * r->p_picture->p->i_pitch;
        const size_t pixels_offset =
                r->p_picture->format.i_y_offset * r->p_picture->p->i_pitch +
                r->p_picture->format.i_x_offset * r->p_picture->p->i_pixel_pitch;

        CFDataRef data = CFDataCreate(
            NULL,
            r->p_picture->p->p_pixels + pixels_offset,
            length - pixels_offset);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGImageRef image = CGImageCreate(
            r->p_picture->format.i_visible_width, r->p_picture->format.i_visible_height,
            8, 32, r->p_picture->p->i_pitch,
            space, kCGBitmapByteOrderDefault | kCGImageAlphaFirst,
            provider, NULL, true, kCGRenderingIntentDefault
            );
        VLCSampleBufferSubpictureRegion *region;
        region = [VLCSampleBufferSubpictureRegion new];
        region.subpicture = sys.subpicture;
        region.image = image;
        region.alpha = r->i_alpha / 255.f;

        region.backingFrame = RegionBackingFrame(vd->cfg->display.height, r);
        [regions addObject:region];
        CGDataProviderRelease(provider);
        CFRelease(data);
    }
    CGColorSpaceRelease(space);

    sys.subpicture.regions = regions;
}

static bool IsSubpictureDrawNeeded(vout_display_t *vd, const vlc_render_subpicture *subpicture)
{
    VLCSampleBufferDisplay *sys;
    sys = (__bridge VLCSampleBufferDisplay*)vd->sys;

    if (subpicture == NULL)
    {
        if (sys.subpicture == nil)
            return false;
        sys.subpicture = nil;
        /* Need to draw one last time in order to clear the current subpicture */
        return true;
    }

    size_t count = subpicture->regions.size;
    const struct subpicture_region_rendered *r;

    if (!sys.subpicture || subpicture->i_order != sys.subpicture.order)
    {
        /* Subpicture content is different */
        sys.subpicture = [VLCSampleBufferSubpicture new];
        sys.subpicture.sys = sys;
        sys.subpicture.order = subpicture->i_order;
        UpdateSubpictureRegions(vd, subpicture);
        return true;
    }

    bool draw = false;

    if (count == sys.subpicture.regions.count)
    {
        size_t i = 0;
        vlc_vector_foreach(r, &subpicture->regions)
        {
            VLCSampleBufferSubpictureRegion *region =
                sys.subpicture.regions[i++];

            CGRect newRegion = RegionBackingFrame(vd->cfg->display.height, r);

            if ( !CGRectEqualToRect(region.backingFrame, newRegion) )
            {
                /* Subpicture regions are different */
                draw = true;
                break;
            }
        }
    }
    else
    {
        /* Subpicture region count is different */
        draw = true;
    }

    if (!draw)
        return false;

    /* Store the current subpicture regions in order to compare then later.
     */

    UpdateSubpictureRegions(vd, subpicture);
    return true;
}

static void RenderSubpicture(vout_display_t *vd, const vlc_render_subpicture *spu)
{
    VLCSampleBufferDisplay *sys;
    sys = (__bridge VLCSampleBufferDisplay*)vd->sys;
    if (sys.pictureInPictureStarted)
        return;

    if (!IsSubpictureDrawNeeded(vd, spu))
        return;

    dispatch_async(dispatch_get_main_queue(), ^{
        [sys.spuView drawSubpicture:sys.subpicture];
    });
}

static void PrepareDisplay (vout_display_t *vd) {
    VLCSampleBufferDisplay *sys;
    sys = (__bridge VLCSampleBufferDisplay*)vd->sys;

    [sys prepareDisplay];
}

static void Prepare (vout_display_t *vd, picture_t *pic,
                     const vlc_render_subpicture *subpicture, vlc_tick_t date)
{
    PrepareDisplay(vd);
    if (pic) {
        RenderPicture(vd, pic, date);
    }

    RenderSubpicture(vd, subpicture);
}

static void Display(vout_display_t *vd, picture_t *pic)
{
    // kept as the core is not properly pacing the calls to Prepare without this callback
}

static int PlacementChanged(vout_display_t *vd, const vout_display_place_t *place)
{
    VLCSampleBufferDisplay *sys;
    sys = (__bridge VLCSampleBufferDisplay*)vd->sys;

    vout_display_place_t newPlace = *place;
    dispatch_async(dispatch_get_main_queue(), ^{
        [sys placeVideo:newPlace];
    });

    return VLC_SUCCESS;
}

static pip_controller_t * CreatePipController( vout_display_t *vd, void *cbs_opaque,
                                               void (*state_changed_cb)(void *, bool) )
{
    pip_controller_t *pip_controller = vlc_object_create(vd, sizeof(pip_controller_t));
    pip_controller->state_cb_opaque = cbs_opaque;
    pip_controller->state_changed_cb = state_changed_cb;

    module_t **mods;
    ssize_t total = vlc_module_match("pictureinpicture", NULL, false, &mods, NULL);
    for (ssize_t i = 0; i < total; ++i)
    {
        int (*open)(pip_controller_t *) = vlc_module_map(vd->obj.logger, mods[i]);

        if (open && open(pip_controller) == VLC_SUCCESS)
        {
            free(mods);
            return pip_controller;
        }
    }

    free(mods);
    vlc_object_delete(pip_controller);
    return NULL;
}

static void PictureInPictureStateChanged(void *opaque, bool is_started)
{
    VLCSampleBufferDisplay *sys = (__bridge VLCSampleBufferDisplay *)opaque;
    [sys handlePictureInPictureStateChange:is_started];
}

static void DeletePipController( pip_controller_t * pip_controller )
{
    if (pip_controller == NULL)
        return;

    if( pip_controller->ops->close )
    {
        pip_controller->ops->close(pip_controller);
    }

    vlc_object_delete(pip_controller);
}

static int UpdateFormat(vout_display_t *vd, const video_format_t *fmt,
                        vlc_video_context *vctx)
{
    VLCSampleBufferDisplay *sys = (__bridge VLCSampleBufferDisplay*)vd->sys;

    // Display will only work with CVPX video context
    filter_t *converter = NULL;
    if (!vlc_video_context_GetPrivate(vctx, VLC_VIDEO_CONTEXT_CVPX)) {
        converter = CreateCVPXConverter(vd, fmt);
        if (!converter)
            return VLC_EGENERIC;
    }

    DeleteCVPXConverter(sys->converter);
    sys->converter = converter;
    return VLC_SUCCESS;
}

static int Open (vout_display_t *vd,
                 video_format_t *fmt, vlc_video_context *context)
{
    if (var_InheritBool(vd, "force-darwin-legacy-display")) {
        return VLC_EGENERIC;
    }
    // Display isn't compatible with 360 content hence opening with this kind
    // of projection should fail if display use isn't forced
    if (!vd->obj.force && fmt->projection_mode != PROJECTION_MODE_RECTANGULAR) {
        return VLC_EGENERIC;
    }

    // Display will only work with CVPX video context
    filter_t *converter = NULL;
    if (!vlc_video_context_GetPrivate(context, VLC_VIDEO_CONTEXT_CVPX)) {
        converter = CreateCVPXConverter(vd, fmt);
        if (!converter)
            return VLC_EGENERIC;
    }

    @autoreleasepool {
        VLCSampleBufferDisplay *sys =
            [[VLCSampleBufferDisplay alloc] initWithVoutDisplay:vd];

        if (sys == nil) {
            DeleteCVPXConverter(converter);
            return VLC_ENOMEM;
        }

        sys->converter = converter;

        vd->sys = (__bridge_retained void*)sys;

        static const struct vlc_display_operations ops = {
            .close = Close,
            .prepare = Prepare,
            .display = Display,
            .update_format = UpdateFormat,
            .video_place_changed = PlacementChanged,
        };

        vd->ops = &ops;

        vd->info.subpicture_chromas = sample_buffer_display_subfmts;

        return VLC_SUCCESS;
    }
}

/*
 * Module descriptor
 */

#define FORCE_LEGACY_DISPLAY_TEXT N_("Force fallback to legacy display")
#define FORCE_LEGACY_DISPLAY_LONGTEXT N_( \
    "Triggers an initialization failure to allow fallback to any other legacy display.")
#define STABLE_HDR_TONE_MAPPING_TEXT N_("Use stable HDR-to-SDR tone mapping")
#define STABLE_HDR_TONE_MAPPING_LONGTEXT N_( \
    "On SDR displays, convert HDR10 and HLG frames to SDR Rec. 709. HLG " \
    "midtones retain their original brightness and only highlights use a " \
    "fixed shoulder, avoiding both color washout and brightness pumping. " \
    "HDR-capable displays receive the original HDR signal.")

#define HELP_TEXT N_("This display handles hardware decoded pixel buffers "\
                     "and renders them in a view/layer. "\
                     "It can also convert and display software decoded frame buffers. "\
                     "This is the default display for Apple platforms. "\
                     "--force-darwin-legacy-display option can be used to abort the "\
                     "display's initialization and allows fallback to legacy displays like "\
                     "other OpenGL/ES based video outputs.")

vlc_module_begin()
    set_description(N_("CoreMedia sample buffers based video output display"))
    set_subcategory(SUBCAT_VIDEO_VOUT)
    add_bool("force-darwin-legacy-display", false,
             FORCE_LEGACY_DISPLAY_TEXT, FORCE_LEGACY_DISPLAY_LONGTEXT)
        change_volatile()
    add_bool("samplebuffer-stable-hdr-tone-mapping", true,
             STABLE_HDR_TONE_MAPPING_TEXT,
             STABLE_HDR_TONE_MAPPING_LONGTEXT)
    set_help(HELP_TEXT)
    set_callback_display(Open, 600)
vlc_module_end()
