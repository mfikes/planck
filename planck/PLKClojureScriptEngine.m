#include <unistd.h>
#import "PLKClojureScriptEngine.h"
#import "PLKSh.h"
#import "ABYUtils.h"
#import "ABYContextManager.h"
#import "PLKBundledOut.h"
#import "PLKFileReader.h"
#import "PLKFileWriter.h"
#import "PLKFileInputStream.h"
#import "PLKFileOutputStream.h"
#import "ZZArchive.h"
#import "ZZArchiveEntry.h"
#include "linenoise.h"

@interface PLKClojureScriptEngine()

@property (nonatomic, strong) NSCondition* javaScriptEngineReadyCondition;
@property (nonatomic) BOOL javaScriptEngineReady;
@property (nonatomic, strong) NSCondition* cacheTasksCondition;
@property (nonatomic) int cacheTasksOutstanding;
@property (nonatomic) JSGlobalContextRef context;
@property (nonatomic, strong) ABYContextManager* contextManager;
@property (nonatomic, strong) PLKBundledOut* bundledOut;
@property (nonatomic, strong) NSMutableSet* loadedGoogLibs;
@property (nonatomic) int exitValue;
@property (nonatomic, strong) NSMutableDictionary* openArchives;
@property (nonatomic, strong) NSMutableDictionary* openArchiveModificationDates;

@property (nonatomic) int descriptorSequence;
@property (nonatomic, strong) NSMutableDictionary* descriptorToObject;

@end

@implementation PLKClojureScriptEngine

int JSArrayGetCount(JSContextRef ctx, JSObjectRef arr)
{
    JSStringRef pname = JSStringCreateWithUTF8CString("length");
    JSValueRef val = JSObjectGetProperty(ctx, arr, pname, NULL);
    JSStringRelease(pname);
    return JSValueToNumber(ctx, val, NULL);
}

JSValueRef JSArrayGetValueAtIndex(JSContextRef ctx, JSObjectRef arr, int index)
{
    return JSObjectGetPropertyAtIndex(ctx, arr, index, NULL);
}

JSStringRef JSStringRefFromNSString(NSString* string) {
    return JSStringCreateWithCFString((__bridge CFStringRef)string);
}

JSValueRef JSValueMakeStringFromNSString(JSContextRef ctx, NSString* string)
{
    return string ? JSValueMakeString(ctx, JSStringRefFromNSString(string)) : JSValueMakeNull(ctx);
}

NSString* NSStringFromJSValueRef(JSContextRef ctx, JSValueRef jsValueRef)
{
    if (JSValueIsNull(ctx, jsValueRef)) {
        return nil;
    }
    JSStringRef stringRef = JSValueToStringCopy(ctx, jsValueRef, NULL);
    return (__bridge_transfer NSString *)JSStringCopyCFString(kCFAllocatorDefault, stringRef);
}

-(JSValueRef)registerAndGetDescriptor:(NSObject*)o
{
    NSString* descriptor = [NSString stringWithFormat:@"PLANCK_DESCRIPTOR_%d", ++self.descriptorSequence];
    [self.descriptorToObject setObject:o forKey:descriptor];
    return JSValueMakeStringFromNSString(self.context, descriptor);
}

-(void)initalizeEngineReadyConditionVars
{
    self.javaScriptEngineReadyCondition = [[NSCondition alloc] init];
    self.javaScriptEngineReady = NO;
}

-(void)blockUntilEngineReady
{
    [self.javaScriptEngineReadyCondition lock];
    while (!self.javaScriptEngineReady)
        [self.javaScriptEngineReadyCondition wait];
    
    [self.javaScriptEngineReadyCondition unlock];
}

-(void)signalEngineReady
{
    [self.javaScriptEngineReadyCondition lock];
    self.javaScriptEngineReady = YES;
    [self.javaScriptEngineReadyCondition signal];
    [self.javaScriptEngineReadyCondition unlock];
}

-(void)initalizeCacheTaskConditionVars
{
    self.cacheTasksCondition = [[NSCondition alloc] init];
    self.cacheTasksOutstanding = 0;
}

-(void)blockUntilCacheTasksComplete
{
    [self.cacheTasksCondition lock];
    while (self.cacheTasksOutstanding > 0)
        [self.cacheTasksCondition wait];
    
    [self.cacheTasksCondition unlock];
}

-(void)startCacheTask
{
    [self.cacheTasksCondition lock];
    self.cacheTasksOutstanding++;
    [self.cacheTasksCondition unlock];
}

-(void)signalCacheTaskComplete
{
    [self.cacheTasksCondition lock];
    self.cacheTasksOutstanding--;
    [self.cacheTasksCondition signal];
    [self.cacheTasksCondition unlock];
}

- (void)setUpTimerFunctionality
{
    
    static volatile int32_t counter = 0;
    
    NSString* callbackImpl = @"var callbackstore = {};\nvar setTimeout = function( fn, ms ) {\ncallbackstore[setTimeoutFn(ms)] = fn;\n}\nvar runTimeout = function( id ) {\nif( callbackstore[id] )\ncallbackstore[id]();\ncallbackstore[id] = null;\n}\n";
    
    [ABYUtils evaluateScript:callbackImpl inContext:self.contextManager.context];
    
    [ABYUtils installGlobalFunctionWithBlock:
     
     ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
         if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeNumber)
         {
             int ms = (int)JSValueToNumber(ctx, argv[0], NULL);
             
             int32_t incremented = OSAtomicIncrement32(&counter);
             
             NSString *str = [NSString stringWithFormat:@"timer%d", incremented];
             
             dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ms * NSEC_PER_MSEC), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                 [ABYUtils evaluateScript:[NSString stringWithFormat:@"runTimeout(\"%@\");", str] inContext:self.contextManager.context];
             });
             
             JSStringRef strRef = JSStringCreateWithCFString((__bridge CFStringRef)str);
             JSValueRef rv = JSValueMakeString(ctx, strRef);
             JSStringRelease(strRef);
             return rv;
         }
         
         return JSValueMakeUndefined(ctx);
     }
                                        name:@"setTimeoutFn"
                                     argList:@"ms"
                                   inContext:self.contextManager.context];
}

-(NSDate*)getModificationDateForFile:(NSString*)path
{
    NSDate* fileModificationDate = nil;
    
    NSError* error = nil;
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:&error];
    if (attrs && !error)
    {
        fileModificationDate = [attrs fileModificationDate];
    }
    
    return fileModificationDate;
}

-(void)startInitializationWithSrcPaths:(NSArray*)srcPaths outPath:(NSString*)outPath cachePath:(NSString*)cachePath verbose:(BOOL)verbose staticFns:(BOOL)staticFns boundArgs:(NSArray*)boundArgs planckVersion:(NSString*)planckVersion repl:(BOOL)repl
{
    // By default we expect :none, but this can be set if :simple
    
    BOOL useSimpleOutput = NO;
    
    // Initialize out path / URL
    
    NSURL* outURL = [NSURL URLWithString:@"out"];
    
    if (outPath) {
        outURL = [NSURL URLWithString:outPath];
    }
    
    if (outPath) {
        NSFileManager* fm = [NSFileManager defaultManager];
        if (![fm fileExistsAtPath:outURL.path isDirectory:nil]) {
            NSLog(@"ClojureScript compiler output directory not found at \"%@\".", outURL.path);
            NSLog(@"(Current working directory is \"%@\")", [fm currentDirectoryPath]);
            NSLog(@"If running from Xcode, set -o $PROJECT_DIR/planck-cljs/out");
            exit(1);
        }
    }
    
    // ... and prepare for the "bundled" out location
    
    self.bundledOut = [[PLKBundledOut alloc] init];
    
    self.descriptorToObject = [[NSMutableDictionary alloc] init];
    
    // Now, start initializing JavaScriptCore in a background thread and return
    
    [self initalizeEngineReadyConditionVars];
    [self initalizeCacheTaskConditionVars];
    
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^() {
        
        self.contextManager = [[ABYContextManager alloc] initWithContext:JSGlobalContextCreate(NULL)
                                                 compilerOutputDirectory:outURL];
        [self.contextManager setupGlobalContext];
        [self.contextManager setUpConsoleLog];
        [self setUpTimerFunctionality];
        
        if (!useSimpleOutput) {
            if (outPath) {
                [self.contextManager setUpAmblyImportScript];
            } else {
                self.loadedGoogLibs = [[NSMutableSet alloc] init];
                [self setUpAmblyImportScriptInContext:self.contextManager.context];
            }
        }
        
        NSString* mainJsFilePath = [[outURL URLByAppendingPathComponent:@"main" isDirectory:NO]
                                    URLByAppendingPathExtension:@"js"].path;
        
        NSURL* googDirectory = [outURL URLByAppendingPathComponent:@"goog"];
        
        if (useSimpleOutput) {
            NSString *mainJsString;
            if (outPath) {
                mainJsString = [NSString stringWithContentsOfFile:mainJsFilePath encoding:NSUTF8StringEncoding error:nil];
            } else {
                mainJsString = [self.bundledOut getSourceForPath:@"main.js"];
            }
            NSAssert(mainJsString != nil, @"The main JavaScript text could not be loaded");
            [ABYUtils evaluateScript:mainJsString inContext:self.contextManager.context];
        } else {
            if (outPath) {
                [self.contextManager bootstrapWithDepsFilePath:mainJsFilePath
                                                  googBasePath:[[googDirectory URLByAppendingPathComponent:@"base" isDirectory:NO] URLByAppendingPathExtension:@"js"].path];
            } else {
                [self bootstrapInContext:self.contextManager.context];
            }
        }
        
        self.context = self.contextManager.context;
        
        if (!useSimpleOutput) {
            [self requireAppNamespaces:self.context];
        }
        
        // TODO look into this. Without it thngs won't work.
        [ABYUtils evaluateScript:@"var window = global;" inContext:self.context];
        
        [ABYUtils setValue:JSValueMakeStringFromNSString(self.context, planckVersion)
                  onObject:JSContextGetGlobalObject(self.context)
               forProperty:@"PLANCK_VERSION"
                 inContext:self.context];
        
        self.openArchives = [[NSMutableDictionary alloc] init];
        self.openArchiveModificationDates = [[NSMutableDictionary alloc] init];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 NSString* source = nil;
                 NSDate* sourceFileModified = nil;
                 
                 // First try in the srcPaths
                 
                 if (![path isEqualToString:@"cljs/core.cljs.cache.aot.json"] &&
                     ![path isEqualToString:@"cljs/core$macros.cljc.cache.json"]) {
                     
                     for (NSArray* srcPath in srcPaths) {
                         NSString* type = srcPath[0];
                         NSString* location = srcPath[1];
                         
                         if ([type isEqualToString:@"src"]) {
                             NSString* fullPath = [location stringByAppendingString:path];
                             
                             source = [NSString stringWithContentsOfFile:fullPath
                                                                encoding:NSUTF8StringEncoding error:nil];
                             if (source) {
                                 sourceFileModified = [self getModificationDateForFile:fullPath];
                             }
                         } else if ([type isEqualToString:@"jar"]) {
                             ZZArchive* archive = self.openArchives[location];
                             if (!archive) {
                                 NSError* err = nil;
                                 archive = [ZZArchive archiveWithURL:[NSURL fileURLWithPath:location]
                                                               error:&err];
                                 if (err) {
                                     NSLog(@"%@", err);
                                     self.openArchives[location] = [NSNull null];
                                 } else {
                                     self.openArchives[location] = archive;
                                     self.openArchiveModificationDates[location] = [self getModificationDateForFile:location];
                                 }
                             }
                             NSData* data = nil;
                             if (![archive isEqual:[NSNull null]]) {
                                 for (ZZArchiveEntry* entry in archive.entries)
                                     if ([entry.fileName isEqualToString:path]) {
                                         data = [entry newDataWithError:nil];
                                         break;
                                     }
                             }
                             if (data) {
                                 source = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                                 sourceFileModified = self.openArchiveModificationDates[path];
                             }
                         }
                         if (source) {
                             break;
                         }
                     }
                 }
                 
                 // Now try to load the file from the output
                 if (!source) {
                     if (outPath) {
                         NSString* fullPath = [outPath stringByAppendingString:path];
                         source = [NSString stringWithContentsOfFile:fullPath
                                                            encoding:NSUTF8StringEncoding error:nil];
                         sourceFileModified = [self getModificationDateForFile:fullPath];
                     } else {
                         source = [self.bundledOut getSourceForPath:path];
                         sourceFileModified = [NSDate dateWithTimeIntervalSince1970:0];
                     }
                 }
                 
                 if (source) {
                     JSValueRef arguments[2];
                     arguments[0] = JSValueMakeStringFromNSString(ctx, source);
                     arguments[1] = JSValueMakeNumber(ctx, [sourceFileModified timeIntervalSince1970]);
                     
                     return JSObjectMakeArray(ctx, 2, arguments, NULL);
                 }

             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_LOAD"
                                         argList:@"path"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 4 &&
                 JSValueGetType (ctx, argv[0]) == kJSTypeString &&
                 JSValueGetType (ctx, argv[1]) == kJSTypeString &&
                 (JSValueGetType (ctx, argv[2]) == kJSTypeString
                  || JSValueGetType (ctx, argv[2]) == kJSTypeNull) &&
                 (JSValueGetType (ctx, argv[3]) == kJSTypeString
                  || JSValueGetType (ctx, argv[3]) == kJSTypeNull)) {
                     
                     NSString* cachePrefix = NSStringFromJSValueRef(ctx, argv[0]);
                     NSString* source = NSStringFromJSValueRef(ctx, argv[1]);
                     NSString* cache = NSStringFromJSValueRef(ctx, argv[2]);
                     NSString* sourcemap = NSStringFromJSValueRef(ctx, argv[3]);
                     
                     [self startCacheTask];
                     
                     dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_LOW, 0), ^(){
                         
                         
                         [source writeToFile:[cachePrefix stringByAppendingString:@".js"]
                                  atomically:YES
                                    encoding:NSUTF8StringEncoding
                                       error:nil];
                         [cache writeToFile:[cachePrefix stringByAppendingString:@".cache.json"]
                                 atomically:YES
                                   encoding:NSUTF8StringEncoding
                                      error:nil];
                         [sourcemap writeToFile:[cachePrefix stringByAppendingString:@".js.map.json"]
                                     atomically:YES
                                       encoding:NSUTF8StringEncoding
                                          error:nil];
                         
                         [self signalCacheTaskComplete];
                         
                     });
                 }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_CACHE"
                                         argList:@"path, source, cache, sourcemap"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             JSValueRef rv = NULL;
             
             if (argc == 2 &&
                 JSValueGetType (ctx, argv[0]) == kJSTypeString &&
                 JSValueGetType (ctx, argv[1]) == kJSTypeString) {
                 
                 // Uncomment these if you need to see the values in the debugger
                 // NSString* source = NSStringFromJSValueRef(ctx, argv[0]);
                 // NSString* path = NSStringFromJSValueRef(ctx, argv[1]);
                 
                 JSStringRef sourceRef = JSValueToStringCopy(ctx, argv[0], NULL);
                 JSStringRef pathRef = JSValueToStringCopy(ctx, argv[1], NULL);
                 
                 JSEvaluateScript(ctx, sourceRef, NULL, pathRef, 0, &rv);
                 
                 JSStringRelease(pathRef);
                 JSStringRelease(sourceRef);
             }
             
             return  rv != NULL ?  rv : JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_EVAL"
                                         argList:@"source, path"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* file = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 NSString* contents = [NSString stringWithContentsOfFile:file
                                                                encoding:NSUTF8StringEncoding error:nil];
                 if (contents) {
                     
                     JSValueRef arguments[2];
                     arguments[0] = JSValueMakeStringFromNSString(ctx, contents);
                     arguments[1] = JSValueMakeNumber(ctx, [[self getModificationDateForFile:file] timeIntervalSince1970]);
                     
                     return JSObjectMakeArray(ctx, 2, arguments, NULL);
                 }
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_READ_FILE"
                                         argList:@"file"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* prompt = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 char* pass = getpass(prompt.cString);
                 
                 if (pass != NULL) {
                     return JSValueMakeStringFromNSString(ctx, [NSString stringWithUTF8String:pass]);
                 }
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_READ_PASSWORD"
                                         argList:@"prompt"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 6) {
                 
                 int argsCount = JSArrayGetCount(ctx, (JSObjectRef)argv[0]);
                 NSMutableArray* args = [[NSMutableArray alloc] init];
                 for (int i=0; i<argsCount; i++) {
                     [args addObject:NSStringFromJSValueRef(ctx, JSArrayGetValueAtIndex(ctx, (JSObjectRef)argv[0], i))];
                 }
                 
                 NSString* arg_in = NSStringFromJSValueRef(ctx, argv[1]);
                 NSString *encoding_in = NSStringFromJSValueRef(ctx, argv[2]);
                 NSString *encoding_out = NSStringFromJSValueRef(ctx, argv[3]);
                 NSMutableDictionary *env = nil;
                 if (!JSValueIsNull(ctx, argv[4])) {
                     env = [[NSMutableDictionary alloc] init];
                     for (int i=0; i<JSArrayGetCount(ctx, (JSObjectRef)argv[4]); i++) {
                         JSObjectRef keyVal = (JSObjectRef)JSArrayGetValueAtIndex(ctx, (JSObjectRef)argv[4], i);
                         [env setObject:NSStringFromJSValueRef(ctx, JSArrayGetValueAtIndex(ctx, keyVal, 1))
                                 forKey:NSStringFromJSValueRef(ctx, JSArrayGetValueAtIndex(ctx, keyVal, 0))];
                     }
                 }
                 NSString *dir = NSStringFromJSValueRef(ctx, argv[5]);
                 
                 NSDictionary *result = cljs_shell(args, arg_in, encoding_in, encoding_out, env, dir);
                 
                 JSValueRef arguments[3];
                 arguments[0] = JSValueMakeNumber(self.context, ((NSNumber*)result[@"exit"]).doubleValue);
                 arguments[1] = JSValueMakeStringFromNSString(self.context, result[@"out"]);
                 arguments[2] = JSValueMakeStringFromNSString(self.context, result[@"err"]);
                 
                 return JSObjectMakeArray(ctx, 3, arguments, NULL);
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_SHELL_SH"
                                         argList:@"args, arg_in, encoding_in, encoding_out, env, dir"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
                 
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_DELETE"
                                         argList:@"path"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 BOOL isDir = NO;
                 BOOL result = [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && isDir;
                 
                 return JSValueMakeBoolean(ctx, result);
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_IS_DIRECTORY"
                                         argList:@"path"
                                       inContext:self.context];

        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {

                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 NSFileManager* fileManager = [NSFileManager defaultManager];
                 
                 if (![fileManager fileExistsAtPath:path]) {
                     return JSValueMakeNull(ctx);
                 }

                 JSObjectRef retval = JSObjectMake(ctx, NULL, NULL);
                
                 NSDictionary *result = [fileManager attributesOfItemAtPath:path error:nil];
                 result.count; // this is apparently needed to make sure the dictionary is realized. Go figure...

                 if ([result objectForKey:NSFileAppendOnly] != nil) {
                     bool appendOnly = [ result objectForKey:NSFileAppendOnly ] ;
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("appendonly"), JSValueMakeBoolean(ctx, appendOnly), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileBusy] != nil) {
                     bool fileBusy = [ result objectForKey:NSFileBusy];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("filebusy"), JSValueMakeBoolean(ctx, fileBusy), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileDeviceIdentifier] != nil) {
                     double deviceId = [[ result objectForKey:NSFileDeviceIdentifier] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("deviceid"), JSValueMakeNumber(ctx, deviceId), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileExtensionHidden] != nil) {
                     bool extensionHidden = [ result objectForKey:NSFileExtensionHidden];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("extensionhidden"), JSValueMakeBoolean(ctx, extensionHidden), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileHFSCreatorCode] != nil) {
                     double creatorCode = [[result objectForKey:NSFileHFSCreatorCode] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("creatorcode"), JSValueMakeNumber(ctx, creatorCode), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileHFSTypeCode] != nil) {
                     double typeCode = [[result objectForKey:NSFileHFSTypeCode] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("typecode"), JSValueMakeNumber(ctx, typeCode), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileImmutable] != nil) {
                     bool immutable = [result objectForKey:NSFileImmutable];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("immutable"), JSValueMakeBoolean(ctx, immutable), kJSPropertyAttributeReadOnly, nil);

                 }
                 if ([result objectForKey:NSFilePosixPermissions] != nil) {
                     double permissions = [[result objectForKey:NSFilePosixPermissions] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("permissions"), JSValueMakeNumber(ctx, permissions), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileReferenceCount] != nil) {
                     double referenceCount = [[result objectForKey:NSFileReferenceCount] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("referencecount"), JSValueMakeNumber(ctx, referenceCount), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileSize] != nil) {
                     double filesize = [[result objectForKey:NSFileSize] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("filesize"), JSValueMakeNumber(ctx, filesize), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileSystemFileNumber] != nil) {
                     double filenumber = [[result objectForKey:NSFileSystemFileNumber] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("filenumber"), JSValueMakeNumber(ctx, filenumber), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileType] != nil) {
                     NSString* type = [result objectForKey:NSFileType];
                     NSString * typeString = @"unknown";
                     if ([type isEqualToString:NSFileTypeDirectory]) {
                         typeString = @"directory";
                     } else if ([type isEqualToString:NSFileTypeRegular]) {
                         typeString = @"file";
                     } else if ([type isEqualToString:NSFileTypeSymbolicLink]) {
                         typeString = @"symboliclink";
                     } else if ([type isEqualToString:NSFileTypeSocket]) {
                         typeString = @"socket";
                     } else if ([type isEqualToString:NSFileTypeCharacterSpecial]) {
                         typeString = @"characterspecial";
                     } else if ([type isEqualToString:NSFileTypeBlockSpecial]) {
                         typeString = @"blockspecial";
                     }
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("type"), JSValueMakeStringFromNSString(ctx, typeString), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileCreationDate] != nil) {
                     NSDate* created = [result objectForKey:NSFileCreationDate];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("created"), JSValueMakeNumber(ctx, 1000.0*[created timeIntervalSince1970]), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileModificationDate] != nil) {
                     NSDate* modified = [result objectForKey:NSFileModificationDate];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("modified"), JSValueMakeNumber(ctx, 1000.0*[modified timeIntervalSince1970]), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileOwnerAccountID] != nil) {
                     double uid = [[result objectForKey:NSFileOwnerAccountID] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("uid"),  JSValueMakeNumber(ctx, uid), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileOwnerAccountName] != nil) {
                     NSString* owner = [result objectForKey:NSFileOwnerAccountName];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("uname"), JSValueMakeStringFromNSString(ctx, owner), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileGroupOwnerAccountName] != nil) {
                     NSString* groupname = [result objectForKey:NSFileGroupOwnerAccountName];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("gname"), JSValueMakeStringFromNSString(ctx, groupname), kJSPropertyAttributeReadOnly, nil);
                 }
                 if ([result objectForKey:NSFileGroupOwnerAccountID] != nil) {
                     double gid = [[result objectForKey:NSFileGroupOwnerAccountID] doubleValue];
                     JSObjectSetProperty(ctx, retval, JSStringCreateWithUTF8CString("gid"),  JSValueMakeNumber(ctx, gid), kJSPropertyAttributeReadOnly, nil);
                 }

                 return retval;
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FSTAT"
                                         argList:@"path"
                                       inContext:self.context];
	
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 NSArray *dirFiles = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:path error:nil];
                 
                 JSValueRef  arguments[dirFiles.count];
                 int num_arguments = (int)dirFiles.count;
                 for (int i=0; i<num_arguments; i++) {
                     arguments[i] = JSValueMakeStringFromNSString(self.context, [NSString stringWithFormat:@"%@/%@", path, dirFiles[i]]);
                 }
                 
                 return JSObjectMakeArray(self.context, num_arguments, arguments, NULL);
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_LIST_FILES"
                                         argList:@"path"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* message = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 fprintf(stdout, "%s", [message cStringUsingEncoding:NSUTF8StringEncoding]);
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_RAW_WRITE_STDOUT"
                                         argList:@"message"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             fflush(stdout);
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_RAW_FLUSH_STDOUT"
                                         argList:@""
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* message = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 fprintf(stderr, "%s", [message cStringUsingEncoding:NSUTF8StringEncoding]);
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_RAW_WRITE_STDERR"
                                         argList:@"message"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             fflush(stderr);
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_RAW_FLUSH_STDERR"
                                         argList:@""
                                       inContext:self.context];
        
        __weak typeof(self) weakSelf = self;
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeNumber) {
                 
                 weakSelf.exitValue = JSValueToNumber(ctx, argv[0], NULL);
                 
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_SET_EXIT_VALUE"
                                         argList:@"exitValue"
                                       inContext:self.context];
        
        {
            JSValueRef  arguments[boundArgs.count];
            int num_arguments = (int)boundArgs.count;
            for (int i=0; i<num_arguments; i++) {
                arguments[i] = JSValueMakeStringFromNSString(self.context, boundArgs[i]);
            }
            
            [ABYUtils setValue:JSObjectMakeArray(self.context, num_arguments, arguments, NULL)
                      onObject:JSContextGetGlobalObject(self.context)
                   forProperty:@"PLANCK_INITIAL_COMMAND_LINE_ARGS" inContext:self.context];
        }
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             return JSValueMakeNumber(ctx, weakSelf.exitValue);
         }
                                            name:@"PLANCK_GET_EXIT_VALUE"
                                         argList:@""
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 2 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 NSString* encoding = NSStringFromJSValueRef(ctx, argv[1]);
                 
                 return [self registerAndGetDescriptor:[PLKFileReader open:path encoding:encoding]];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_READER_OPEN"
                                         argList:@"path, encoding"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 PLKFileReader* fileReader = self.descriptorToObject[descriptor];
                 
                 if (!fileReader) {
                     JSValueRef arguments[2];
                     arguments[0] = JSValueMakeNull(ctx);
                     arguments[1] = JSValueMakeStringFromNSString(ctx, @"File closed.");
                     
                     return JSObjectMakeArray(ctx, 2, arguments, NULL);
                 }
                 
                 NSError* readError = nil;
                 NSString* result = [fileReader readWithError:&readError];

                 JSValueRef arguments[2];
                 arguments[0] = JSValueMakeStringFromNSString(ctx, result);
                 arguments[1] = JSValueMakeStringFromNSString(ctx, readError.localizedDescription);

                 return JSObjectMakeArray(ctx, 2, arguments, NULL);
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_READER_READ"
                                         argList:@"descriptor"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 PLKFileReader* fileReader = self.descriptorToObject[descriptor];
                 
                 [fileReader close];
                 
                 [self.descriptorToObject removeObjectForKey:descriptor];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_READER_CLOSE"
                                         argList:@"descriptor"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 3 && JSValueGetType (ctx, argv[0]) == kJSTypeString && JSValueGetType (ctx, argv[1]) == kJSTypeBoolean) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 BOOL append = JSValueToBoolean(ctx, argv[1]);
                 NSString* encoding = NSStringFromJSValueRef(ctx, argv[2]);
                 
                 return [self registerAndGetDescriptor:[PLKFileWriter open:path append:append encoding:encoding]];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_WRITER_OPEN"
                                         argList:@"path, append, encoding"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             NSError* writeError = nil;
             if (argc == 2 && JSValueGetType (ctx, argv[0]) == kJSTypeString && JSValueGetType (ctx, argv[1]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 NSString* content = NSStringFromJSValueRef(ctx, argv[1]);
                 
                 PLKFileWriter* fileWriter = self.descriptorToObject[descriptor];
                 
                 if (!fileWriter) {
                     return JSValueMakeStringFromNSString(ctx, @"File closed.");
                 }
                 
                 [fileWriter write:content error:&writeError];
             }
             
             return JSValueMakeStringFromNSString(ctx, writeError.localizedDescription);
         }
                                            name:@"PLANCK_FILE_WRITER_WRITE"
                                         argList:@"descriptor, content"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 PLKFileWriter* fileWriter = self.descriptorToObject[descriptor];
                 
                 [fileWriter close];
                 
                 [self.descriptorToObject removeObjectForKey:descriptor];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_WRITER_CLOSE"
                                         argList:@"descriptor"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 return [self registerAndGetDescriptor:[PLKFileInputStream open:path]];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_INPUT_STREAM_OPEN"
                                         argList:@"path"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 PLKFileInputStream* fileInputStream = self.descriptorToObject[descriptor];
                 
                 NSData* data = [fileInputStream read];
                 uint8_t buf[data.length];
                 [data getBytes:buf length:data.length];
                 
                 JSValueRef  arguments[data.length];
                 int num_arguments = (int)data.length;
                 for (int i=0; i<num_arguments; i++) {
                     arguments[i] = JSValueMakeNumber(ctx, buf[i]);
                 }
                 
                 return JSObjectMakeArray(self.context, num_arguments, arguments, NULL);
                 
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_INPUT_STREAM_READ"
                                         argList:@"descriptor"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 PLKFileInputStream* fileInputStream = self.descriptorToObject[descriptor];
                 
                 [fileInputStream close];
                 
                 [self.descriptorToObject removeObjectForKey:descriptor];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_INPUT_STREAM_CLOSE"
                                         argList:@"descriptor"
                                       inContext:self.context];
        
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 2 && JSValueGetType (ctx, argv[0]) == kJSTypeString && JSValueGetType (ctx, argv[1]) == kJSTypeBoolean) {
                 
                 NSString* path = NSStringFromJSValueRef(ctx, argv[0]);
                 BOOL append = JSValueToBoolean(ctx, argv[1]);
                 
                 return [self registerAndGetDescriptor:[PLKFileOutputStream open:path append:append]];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_OUTPUT_STREAM_OPEN"
                                         argList:@"path, append"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 2 && JSValueGetType (ctx, argv[0]) == kJSTypeString && JSValueGetType (ctx, argv[1]) == kJSTypeObject) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 int count = JSArrayGetCount(ctx, argv[1]);
                 uint8_t buf[count];
                 for (int i=0; i<count; i++) {
                     JSValueRef v = JSArrayGetValueAtIndex(ctx, argv[1], i);
                     if (JSValueIsNumber(ctx, v)) {
                         double n = JSValueToNumber(ctx, v, NULL);
                         if (0 <= n && n <=255) {
                             buf[i] = n;
                         } else {
                             NSLog(@"Output stream value out of range %f", n);
                         }
                     } else {
                         NSLog(@"Output stream value not a number");
                     }
                 }
                 
                 PLKFileOutputStream* fileOutputStream = self.descriptorToObject[descriptor];
                 
                 [fileOutputStream write:[[NSData alloc] initWithBytes:buf length:count]];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_OUTPUT_STREAM_WRITE"
                                         argList:@"descriptor, content"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* descriptor = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 PLKFileOutputStream* fileOutputStream = self.descriptorToObject[descriptor];
                 
                 [fileOutputStream close];
                 
                 [self.descriptorToObject removeObjectForKey:descriptor];
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_FILE_OUTPUT_STREAM_CLOSE"
                                         argList:@"descriptor"
                                       inContext:self.context];

        // Set up to read from stdin
        [self setToReadFrom:nil];
        [self setToPrintOnSender:^(NSString* msg){}];

        {
            JSValueRef  arguments[4];
            int num_arguments = 4;
            arguments[0] = JSValueMakeBoolean(self.context, repl);
            arguments[1] = JSValueMakeBoolean(self.context, verbose);
            arguments[2] = JSValueMakeStringFromNSString(self.context, cachePath);
            arguments[3] = JSValueMakeBoolean(self.context, staticFns);
            JSObjectCallAsFunction(self.context, [self getFunction:@"init"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
        }
        
        // Set up REPL requires
        if (repl) {
            [self executeSourceType:@"text"
                              value:@"(require '[planck.repl :refer-macros [apropos dir doc source pst]])"
                         expression:YES
                 printNilExpression:NO
                      inExitContext:NO
                              setNs:@"cljs.user"
                    blockUntilReady:NO];
        }

        [self setToPrintOnSender:nil];
        
        // Set up the cljs.user namespace
        
        [ABYUtils evaluateScript:@"goog.provide('cljs.user')" inContext:self.context];
        [ABYUtils evaluateScript:@"goog.require('cljs.core')" inContext:self.context];

        // Go for launch!
        
        [self signalEngineReady];
        
        
    });
}

-(void)awaitShutdown
{
    [self blockUntilCacheTasksComplete];
}

-(void)requireAppNamespaces:(JSContextRef)context
{
    [ABYUtils evaluateScript:[NSString stringWithFormat:@"goog.require('%@');", [self munge:@"planck.repl"]]
                   inContext:context];
}

- (JSValueRef)getValue:(NSString*)name inNamespace:(NSString*)namespace fromContext:(JSGlobalContextRef)context
{
    JSValueRef namespaceValue = nil;
    for (NSString* namespaceElement in [namespace componentsSeparatedByString: @"."]) {
        if (namespaceValue) {
            namespaceValue = [ABYUtils getValueOnObject:JSValueToObject(context, namespaceValue, NULL)
                                            forProperty:[self munge:namespaceElement]
                                              inContext:context];
        } else {
            
            namespaceValue = [ABYUtils getValueOnObject:JSContextGetGlobalObject(context)
                                            forProperty:[self munge:namespaceElement]
                                              inContext:context];
        }
    }
    
    return [ABYUtils getValueOnObject:JSValueToObject(context, namespaceValue, NULL)
                          forProperty:[self munge:name]
                            inContext:context];
}

-(NSString*)munge:(NSString*)s
{
    return [[[s stringByReplacingOccurrencesOfString:@"-" withString:@"_"]
             stringByReplacingOccurrencesOfString:@"!" withString:@"_BANG_"]
            stringByReplacingOccurrencesOfString:@"?" withString:@"_QMARK_"];
}

// This implementation is just like Ambly's apart from the canonicalization "goog/../"
// and delegation to CljsRuntime

-(void)setUpAmblyImportScriptInContext:(JSContextRef)context
{
    [ABYUtils installGlobalFunctionWithBlock:
     
     ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
         
         if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString)
         {
             JSStringRef pathStrRef = JSValueToStringCopy(ctx, argv[0], NULL);
             NSString* path = (__bridge_transfer NSString *) JSStringCopyCFString( kCFAllocatorDefault, pathStrRef );
             
             JSStringRelease(pathStrRef);
             
             NSString* url = [@"file:///" stringByAppendingString:path];
             
             JSStringRef urlStringRef = JSStringCreateWithCFString((__bridge CFStringRef)url);
             
             BOOL canSkipLoad = NO;
             if ([path hasPrefix:@"goog/../"]) {
                 path = [path substringFromIndex:8];
             } else {
                 if ([self.loadedGoogLibs containsObject:path]) {
                     canSkipLoad = YES;
                 } else {
                     [self.loadedGoogLibs addObject:path];
                 }
             }
             
             if (!canSkipLoad) {
                 
                 NSError* error = nil;
                 NSString* sourceText = [self.bundledOut getSourceForPath:path];
                 
                 if (!error && sourceText) {
                     
                     JSValueRef jsError = NULL;
                     JSStringRef javaScriptStringRef = JSStringCreateWithCFString((__bridge CFStringRef)sourceText);
                     JSEvaluateScript(ctx, javaScriptStringRef, NULL, urlStringRef, 0, &jsError);
                     JSStringRelease(javaScriptStringRef);
                 }
             }
             
             JSStringRelease(urlStringRef);
         }
         
         return JSValueMakeUndefined(ctx);
     }
                                        name:@"AMBLY_IMPORT_SCRIPT"
                                     argList:@"path"
                                   inContext:context];
    
}

// This implementation is just like Ambly's apart from the delegation to CljsRuntime

-(void)bootstrapInContext:(JSContextRef)context
{
    // This implementation mirrors the bootstrapping code that is in -setup
    
    // Setup CLOSURE_IMPORT_SCRIPT
    [ABYUtils evaluateScript:@"CLOSURE_IMPORT_SCRIPT = function(src) { AMBLY_IMPORT_SCRIPT('goog/' + src); return true; }" inContext:context];
    
    // Load goog base
    NSString *baseScriptString = [self.bundledOut getSourceForPath:@"goog/base.js"];
    NSAssert(baseScriptString != nil, @"The goog base JavaScript text could not be loaded");
    [ABYUtils evaluateScript:baseScriptString inContext:context];
    
    // Load the deps file
    NSString *depsScriptString = [self.bundledOut getSourceForPath:@"main.js"];
    NSAssert(depsScriptString != nil, @"The deps JavaScript text could not be loaded");
    [ABYUtils evaluateScript:depsScriptString inContext:context];
    
    [ABYUtils evaluateScript:@"goog.isProvided_ = function(x) { return false; };" inContext:context];
    
    [ABYUtils evaluateScript:@"goog.require = function (name) { return CLOSURE_IMPORT_SCRIPT(goog.dependencies_.nameToPath[name]); };" inContext:context];
    
    [ABYUtils evaluateScript:@"goog.require('cljs.core');" inContext:context];
    
    // redef goog.require to track loaded libs
    [ABYUtils evaluateScript:@"cljs.core._STAR_loaded_libs_STAR_ = cljs.core.into.call(null, cljs.core.PersistentHashSet.EMPTY, [\"cljs.core\"]);\n"
     "goog.require = function (name, reload) {\n"
     "    if(!cljs.core.contains_QMARK_(cljs.core._STAR_loaded_libs_STAR_, name) || reload) {\n"
     "        var AMBLY_TMP = cljs.core.PersistentHashSet.EMPTY;\n"
     "        if (cljs.core._STAR_loaded_libs_STAR_) {\n"
     "            AMBLY_TMP = cljs.core._STAR_loaded_libs_STAR_;\n"
     "        }\n"
     "        cljs.core._STAR_loaded_libs_STAR_ = cljs.core.into.call(null, AMBLY_TMP, [name]);\n"
     "        CLOSURE_IMPORT_SCRIPT(goog.dependencies_.nameToPath[name]);\n"
     "    }\n"
     "};" inContext:context];
}

-(void)setPrintFnsInContext:(JSContextRef)context
{
    [ABYUtils evaluateScript:@"cljs.core.set_print_fn_BANG_.call(null,PLANCK_PRINT_FN);" inContext:context];
    [ABYUtils evaluateScript:@"cljs.core.set_print_err_fn_BANG_.call(null,PLANCK_PRINT_ERR_FN);" inContext:context];
}

-(JSObjectRef)getFunction:(NSString*)name
{
    JSValueRef rv = [self getValue:name inNamespace:@"planck.repl" fromContext:self.context];
    NSAssert(!JSValueIsUndefined(self.context, rv) , name);
    return JSValueToObject(self.context, rv, NULL);
}

-(int)executeSourceType:(NSString*)sourceType value:(NSString*)sourceValue expression:(BOOL)expression printNilExpression:(BOOL)printNilExpression inExitContext:(BOOL)inExitContext setNs:(NSString*)setNs blockUntilReady:(BOOL)blockUntilReady
{
    if (blockUntilReady) {
        [self blockUntilEngineReady];
    }
    
    if (!inExitContext) {
        // Default return value will indicate non-terminating successful exit
        self.exitValue = PLANK_EXIT_SUCCESS_NONTERMINATE;
    } else {
        self.exitValue = EXIT_SUCCESS;
    }
    
    JSValueRef  arguments[5];
    JSValueRef result;
    int num_arguments = 5;
    
    {
        JSValueRef  sourceArguments[2];
        sourceArguments[0] = JSValueMakeStringFromNSString(self.context, sourceType);
        sourceArguments[1] = JSValueMakeStringFromNSString(self.context, sourceValue);
        arguments[0] = JSObjectMakeArray(self.context, 2, sourceArguments, NULL);
    }
    
    arguments[1] = JSValueMakeBoolean(self.context, expression);
    arguments[2] = JSValueMakeBoolean(self.context, printNilExpression);
    arguments[3] = JSValueMakeBoolean(self.context, inExitContext);
    arguments[4] = JSValueMakeStringFromNSString(self.context, setNs);
    result = JSObjectCallAsFunction(self.context, [self getFunction:@"execute"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
    
    return self.exitValue;
}

-(int)runMainInNs:(NSString*)mainNsName args:(NSArray*)args
{
    [self blockUntilEngineReady];
    self.exitValue = EXIT_SUCCESS;
    
    JSValueRef  arguments[args.count+1];
    JSValueRef result;
    int num_arguments = (int)args.count+1;
    arguments[0] = JSValueMakeStringFromNSString(self.context, mainNsName);
    for (int i=1; i<num_arguments; i++) {
        arguments[i] = JSValueMakeStringFromNSString(self.context, args[i-1]);
    }
    
    result = JSObjectCallAsFunction(self.context, [self getFunction:@"run-main"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
    
    return self.exitValue;
}

-(NSString*)isReadable:(NSString*)expression
{
    [self blockUntilEngineReady];
    JSValueRef  arguments[1];
    JSValueRef result;
    int num_arguments = 1;
    arguments[0] = JSValueMakeStringFromNSString(self.context, expression);
    result = JSObjectCallAsFunction(self.context, [self getFunction:@"is-readable?"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
    return NSStringFromJSValueRef(self.context, result);
}

-(NSString*)getCurrentNs
{
    [self blockUntilEngineReady];
    JSValueRef  arguments[0];
    JSValueRef result;
    int num_arguments = 0;
    result = JSObjectCallAsFunction(self.context, [self getFunction:@"get-current-ns"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
    JSStringRef currentNsStringRef = JSValueToStringCopy(self.context, result, NULL);
    NSString* currentNs = (__bridge_transfer NSString *)JSStringCopyCFString(kCFAllocatorDefault, currentNsStringRef);
    return currentNs;
}

-(NSArray*)getCompletionsForBuffer:(NSString*)buffer
{
    [self blockUntilEngineReady];
    
    JSValueRef  arguments[1];
    arguments[0] = JSValueMakeStringFromNSString(self.context, buffer);
    JSValueRef result;
    int num_arguments = 1;
    result = JSObjectCallAsFunction(self.context, [self getFunction:@"get-completions"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
    NSMutableArray* results = [[NSMutableArray alloc] init];
    for (int i=0; i<JSArrayGetCount(self.context, result); i++) {
        [results addObject:NSStringFromJSValueRef(self.context, JSArrayGetValueAtIndex(self.context, result, i))];
    }
    return results;
}

-(NSArray*)getHighlightCoordsForPos:(int)pos buffer:(NSString*)buffer previousLines:(NSArray*)previousLines
{
    [self blockUntilEngineReady];
    JSValueRef  arguments[3];
    arguments[0] = JSValueMakeNumber(self.context, pos);
    arguments[1] = JSValueMakeStringFromNSString(self.context, buffer);
    {
        JSValueRef prevLines[previousLines.count];
        for (int i=0; i<previousLines.count; i++) {
            prevLines[i] = JSValueMakeStringFromNSString(self.context, previousLines[i]);
        }
        arguments[2] = JSObjectMakeArray(self.context, previousLines.count, prevLines, NULL);
    }
    JSValueRef result;
    int num_arguments = 3;
    result = JSObjectCallAsFunction(self.context, [self getFunction:@"get-highlight-coords"], JSContextGetGlobalObject(self.context), num_arguments, arguments, NULL);
    
    return @[@((int)JSValueToNumber(self.context, JSArrayGetValueAtIndex(self.context, result, 0), NULL)),
             @((int)JSValueToNumber(self.context, JSArrayGetValueAtIndex(self.context, result, 1), NULL))];
}


-(void)setToPrintOnSender:(void (^)(NSString*))sender {
    
    if (sender) {
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* message = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 sender(message);
                 
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_PRINT_FN"
                                         argList:@"message"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* message = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 sender(message);
                 
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_PRINT_ERR_FN"
                                         argList:@"message"
                                       inContext:self.context];
    } else {
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* message = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 fprintf(stdout, "%s", [message cStringUsingEncoding:NSUTF8StringEncoding]);
                 fflush(stdout);
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_PRINT_FN"
                                         argList:@"message"
                                       inContext:self.context];
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             if (argc == 1 && JSValueGetType (ctx, argv[0]) == kJSTypeString) {
                 
                 NSString* message = NSStringFromJSValueRef(ctx, argv[0]);
                 
                 fprintf(stderr, "%s", [message cStringUsingEncoding:NSUTF8StringEncoding]);
                 
             }
             
             return JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_PRINT_ERR_FN"
                                         argList:@"message"
                                       inContext:self.context];
    }
    
    [self setPrintFnsInContext:self.contextManager.context];
}

-(void)setToReadFrom:(NSString* (^)())input {
    
    if (input) {
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             NSString* string = input();
             return JSValueMakeStringFromNSString(ctx, string);
             //return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_RAW_READ_STDIN"
                                         argList:@""
                                       inContext:self.context];
    } else {
        const BOOL isTty = isatty(fileno(stdin));
        
        [ABYUtils installGlobalFunctionWithBlock:
         ^JSValueRef(JSContextRef ctx, size_t argc, const JSValueRef argv[]) {
             
             NSMutableData *data=[[NSMutableData alloc] init];
             
             NSFileHandle *input = [NSFileHandle fileHandleWithStandardInput];
             NSData *inputData = [input readDataOfLength:isTty ? 1 : 1024];
             if (inputData.length) {
                 
                 [data appendData:inputData];
                 
                 NSString *string = [[NSString alloc] initWithData:data
                                                          encoding:NSUTF8StringEncoding];
                 if (string) {
                     return JSValueMakeStringFromNSString(ctx, string);
                 } else {
                     // Couldn't decode UTF8. Try reading up to 6 more bytes to see if
                     // we can form a well-formed UTF8 string
                     int tries = 6;
                     while (tries-- > 0) {
                         inputData = [input readDataOfLength:1];
                         if (inputData.length > 0) {
                             [data appendData:inputData];
                             NSString *string = [[NSString alloc] initWithData:data
                                                                      encoding:NSUTF8StringEncoding];
                             if (string) {
                                 return JSValueMakeStringFromNSString(ctx, string);
                             }
                         } else {
                             NSLog(@"Failed to decode.");
                             return JSValueMakeNull(ctx);
                         }
                     }
                     
                 }
                 
             }
             
             return  JSValueMakeNull(ctx);
         }
                                            name:@"PLANCK_RAW_READ_STDIN"
                                         argList:@""
                                       inContext:self.context];
    }
}

@end
