#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <android/log.h>
#include <player.h>
#define LOG_TAG "drovik"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)

#ifndef NELEM
#define NELEM(x) ((int)(sizeof(x) / sizeof((x)[0])))
#endif

static const char *s_class_path_name = "com/sky/drovik/player/ffmpeg/JniUtils";

static JNINativeMethod s_methods[] = {
	{"openVideoFile", "(Ljava/lang/String;)I", (void*) openVideoFile},
	//{"", "()V", (void*) glRender},
	//{"glDecode", "(Ljava/lang/String;)V", (void*) glDecode},
	//{"glH264Decoder","([BI)I", (int)glH264Decoder},
	//{"glTakePicture","(Ljava/lang/String;)V", (void*)glTakePicture},
	{"display","(Landroid/graphics/Bitmap;)I", (void *)display}
	//{"ffmpegMpeg4DecoderExit","()V", (void*)ffmpegMpeg4DecoderExit}
	};

static int register_native_methods(JNIEnv* env,
		const char* class_name,
		JNINativeMethod* methods,
		int num_methods)
{
	jclass clazz;

	clazz = (*env)->FindClass(env, class_name);
	if (clazz == NULL) {
		//fprintf(stderr, "Native registration unable to find class '%s'\n", class_name);
		LOGI("Native registration unable to find class '%s'\n",
				class_name);
		return JNI_FALSE;
	}
	if ((*env)->RegisterNatives(env, clazz, methods, num_methods) < 0) {
		//fprintf(stderr, "RegisterNatives failed for '%s'\n", class_name);
		LOGI("RegisterNatives failed for '%s'\n", class_name);
		return JNI_FALSE;
	}
	//LOGI("RegisterNatives success!");
	return JNI_TRUE;
}

static int register_natives(JNIEnv *env)
{
	return register_native_methods(env,
			s_class_path_name,
			s_methods,
			NELEM(s_methods));
}

jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved UNUSED)
{
	JNIEnv* env = NULL;
	jint result = -1;

	if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
		//fprintf(stderr, "ERROR: GetEnv failed\n");
		LOGI("ERROR: GetEnv failed\n");
		goto bail;
	}
	assert(env != NULL);
	int len = sizeof(s_methods) / sizeof(s_methods[0]);   
	if (register_natives(env) < 0) {
		//fprintf(stderr, "ERROR: Exif native registration failed\n");
		LOGI("ERROR: Exif native registration failed\n");
		goto bail;
	}
	/* success -- return valid version number */
	result = JNI_VERSION_1_4;
bail:
	return result;
}

