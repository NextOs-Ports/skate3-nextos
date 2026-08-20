/*
 * jni_slots.h -- JNINativeInterface slot numbers.
 *
 * The order is fixed by the JNI specification. Native Android objects address
 * this table by byte offset, so jni.c asserts the NewStringUTF position at
 * start-up as an ABI check.
 */

#ifndef BC_JNI_SLOTS_H
#define BC_JNI_SLOTS_H

enum {
    JNI_reserved0 = 0,
    JNI_GetVersion = 4,
    JNI_DefineClass,
    JNI_FindClass,
    JNI_FromReflectedMethod,
    JNI_FromReflectedField,
    JNI_ToReflectedMethod,
    JNI_GetSuperclass,
    JNI_IsAssignableFrom,
    JNI_ToReflectedField,
    JNI_Throw,
    JNI_ThrowNew,
    JNI_ExceptionOccurred,
    JNI_ExceptionDescribe,
    JNI_ExceptionClear,
    JNI_FatalError,
    JNI_PushLocalFrame,
    JNI_PopLocalFrame,
    JNI_NewGlobalRef,
    JNI_DeleteGlobalRef,
    JNI_DeleteLocalRef,
    JNI_IsSameObject,
    JNI_NewLocalRef,
    JNI_EnsureLocalCapacity,
    JNI_AllocObject,
    JNI_NewObject,
    JNI_NewObjectV,
    JNI_NewObjectA,
    JNI_GetObjectClass,
    JNI_IsInstanceOf,
    JNI_GetMethodID,                    /* 33 */

    /* 34..93: Call<T>Method{,V,A} then CallNonvirtual<T>Method{,V,A}. */
    JNI_CallObjectMethod = 34,
    JNI_CallNonvirtualObjectMethod = 64,

    JNI_GetFieldID = 94,
    JNI_GetObjectField = 95,            /* +0..8 for Object..Double */
    JNI_SetObjectField = 104,           /* +0..8 */
    JNI_GetStaticMethodID = 113,
    JNI_CallStaticObjectMethod = 114,   /* +0..29 */
    JNI_GetStaticFieldID = 144,
    JNI_GetStaticObjectField = 145,     /* +0..8 */
    JNI_SetStaticObjectField = 154,     /* +0..8 */

    JNI_NewString = 163,
    JNI_GetStringLength,
    JNI_GetStringChars,
    JNI_ReleaseStringChars,
    JNI_NewStringUTF,                   /* 167 -- must stay at 167*8 == 1336 */
    JNI_GetStringUTFLength,
    JNI_GetStringUTFChars,
    JNI_ReleaseStringUTFChars,
    JNI_GetArrayLength,                 /* 171 */
    JNI_NewObjectArray,
    JNI_GetObjectArrayElement,
    JNI_SetObjectArrayElement,
    JNI_NewBooleanArray,                /* 175, +0..7 through Double */
    JNI_GetBooleanArrayElements = 183,  /* +0..7 */
    JNI_ReleaseBooleanArrayElements = 191,
    JNI_GetBooleanArrayRegion = 199,
    JNI_SetBooleanArrayRegion = 207,
    JNI_RegisterNatives = 215,
    JNI_UnregisterNatives,
    JNI_MonitorEnter,
    JNI_MonitorExit,
    JNI_GetJavaVM,
    JNI_GetStringRegion,
    JNI_GetStringUTFRegion,
    JNI_GetPrimitiveArrayCritical,
    JNI_ReleasePrimitiveArrayCritical,
    JNI_GetStringCritical,
    JNI_ReleaseStringCritical,
    JNI_NewWeakGlobalRef,
    JNI_DeleteWeakGlobalRef,
    JNI_ExceptionCheck,
    JNI_NewDirectByteBuffer,
    JNI_GetDirectBufferAddress,
    JNI_GetDirectBufferCapacity,
    JNI_GetObjectRefType,               /* 232 */
    JNI_SLOT_COUNT                      /* 233 */
};

#endif /* BC_JNI_SLOTS_H */
