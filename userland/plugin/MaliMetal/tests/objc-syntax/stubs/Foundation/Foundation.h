#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <objc/runtime.h>
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
typedef unsigned long NSUInteger;
typedef long NSInteger;
#define NS_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define NS_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")
#define NSUIntegerMax ((NSUInteger)-1)
#define NSNotFound ((NSInteger)0x7fffffffffffffff)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define NS_REQUIRES_SUPER
@class NSString, NSError, NSData, NSNumber, NSArray, NSDictionary, NSMutableArray, NSMutableDictionary, NSSet, NSMutableSet, NSMutableString, NSMapTable, NSException;
@protocol NSObject
- (BOOL)isKindOfClass:(Class)c;
- (BOOL)respondsToSelector:(SEL)s;
- (id)copy;
- (id)valueForKey:(NSString *)k;
@end
@protocol NSCopying @end
typedef struct { unsigned long state; __unsafe_unretained id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
@protocol NSFastEnumeration - (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)s objects:(__unsafe_unretained id *)b count:(NSUInteger)n; @end
@interface NSObject <NSObject> { Class isa; }
+ (instancetype)alloc;
+ (instancetype)new;
+ (Class)class;
- (instancetype)init;
- (void)dealloc;
- (BOOL)isEqual:(id)o;
@end
typedef struct _NSRange { NSUInteger location, length; } NSRange;
static inline NSRange NSMakeRange(NSUInteger l, NSUInteger n) { return (NSRange){l, n}; }
@interface NSString : NSObject <NSCopying>
+ (instancetype)stringWithUTF8String:(const char *)s;
+ (instancetype)stringWithFormat:(NSString *)f, ...;
- (instancetype)initWithBytes:(const void *)b length:(NSUInteger)l encoding:(NSUInteger)e;
- (const char *)UTF8String;
- (NSUInteger)length;
- (BOOL)isEqualToString:(NSString *)s;
- (NSData *)dataUsingEncoding:(NSUInteger)e;
- (NSString *)stringByAppendingString:(NSString *)s;
@end
@interface NSMutableString : NSString
+ (instancetype)stringWithCapacity:(NSUInteger)n;
- (void)appendFormat:(NSString *)f, ...;
@end
@interface NSConstantString : NSString @end
#define NSUTF8StringEncoding 4
@interface NSNumber : NSObject
+ (NSNumber *)numberWithInt:(int)v; + (NSNumber *)numberWithUnsignedInt:(unsigned)v; + (NSNumber *)numberWithUnsignedLong:(unsigned long)v;
+ (NSNumber *)numberWithUnsignedLongLong:(unsigned long long)v; + (NSNumber *)numberWithBool:(BOOL)v; + (NSNumber *)numberWithInteger:(NSInteger)v;
+ (NSNumber *)numberWithUnsignedInteger:(NSUInteger)v; + (NSNumber *)numberWithLong:(long)v; + (NSNumber *)numberWithLongLong:(long long)v; + (NSNumber *)numberWithDouble:(double)v;
- (unsigned)unsignedIntValue; - (NSUInteger)unsignedIntegerValue; - (float)floatValue;
@end
@interface NSData : NSObject <NSCopying>
+ (instancetype)dataWithBytes:(const void *)b length:(NSUInteger)l;
+ (instancetype)dataWithBytesNoCopy:(void *)b length:(NSUInteger)l freeWhenDone:(BOOL)f;
- (const void *)bytes; - (NSUInteger)length;
- (NSData *)subdataWithRange:(NSRange)r;
@end
@interface NSMutableData : NSData
+ (instancetype)data; + (instancetype)dataWithLength:(NSUInteger)n;
- (void)appendBytes:(const void *)b length:(NSUInteger)l; - (void *)mutableBytes;
@end
@interface NSArray<__covariant ObjectType> : NSObject <NSCopying, NSFastEnumeration>
+ (instancetype)arrayWithObjects:(const id *)o count:(NSUInteger)c;
- (NSUInteger)count; - (ObjectType)objectAtIndexedSubscript:(NSUInteger)i; - (BOOL)containsObject:(ObjectType)o;
@end
@interface NSMutableArray<ObjectType> : NSArray<ObjectType>
+ (instancetype)array; - (void)addObject:(ObjectType)o; - (void)setObject:(ObjectType)o atIndexedSubscript:(NSUInteger)i;
@end
@interface NSDictionary<__covariant KeyType, __covariant ObjectType> : NSObject <NSCopying>
+ (instancetype)dictionaryWithObjects:(const id *)o forKeys:(const id *)k count:(NSUInteger)c;
- (ObjectType)objectForKeyedSubscript:(KeyType)k;
@end
@interface NSMutableDictionary<KeyType, ObjectType> : NSDictionary<KeyType, ObjectType>
+ (instancetype)dictionary; - (void)setObject:(ObjectType)o forKeyedSubscript:(KeyType)k;
@end
@interface NSSet<__covariant ObjectType> : NSObject
- (BOOL)containsObject:(ObjectType)o; - (NSUInteger)count; - (NSArray<ObjectType> *)allObjects;
@end
@interface NSMutableSet<ObjectType> : NSSet<ObjectType>
+ (instancetype)set; - (void)addObject:(ObjectType)o;
@end
@interface NSMapTable : NSObject
+ (instancetype)strongToWeakObjectsMapTable; + (instancetype)weakToWeakObjectsMapTable;
- (id)objectForKey:(id)k; - (void)setObject:(id)o forKey:(id)k;
@end
@interface NSLock : NSObject
- (void)lock; - (void)unlock;
@end
@interface NSError : NSObject @end
@interface NSException : NSObject
+ (void)raise:(NSString *)n format:(NSString *)f, ...;
- (NSString *)reason;
@end
extern NSString *const NSInternalInconsistencyException, *const NSInvalidArgumentException;
@interface NSJSONSerialization : NSObject
+ (id)JSONObjectWithData:(NSData *)d options:(NSUInteger)o error:(NSError **)e;
@end
SEL NSSelectorFromString(NSString *);
Class NSClassFromString(NSString *);
#include <dispatch/dispatch.h>
static inline id CFBridgingRelease(const void *p) { return (__bridge_transfer id)p; }
const char *getprogname(void);
