/*
 * Copyright (C) 2012, Raviprasad V Mummidi.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "CameraHAL"

// Prevent inclusion of hacky libcamera symbols here
#define THE_WRAPPER

#include <CameraHardwareInterface.h>
#include <hardware/hardware.h>
#include <hardware/camera.h>
#include <binder/IMemory.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/msm_mdp.h>
#include <gralloc_priv.h>
#include <ui/GraphicBufferMapper.h>
#include <dlfcn.h>
#include <utils/Vector.h>

#define NO_ERROR 0

struct blitreq {
   unsigned int count;
   struct mdp_blit_req req;
};

/* Prototypes and extern functions. */
extern "C" android::sp<android::CameraHardwareInterface> HAL_openCameraHardware(int id, int mode);
extern "C" int HAL_getNumberOfCameras();
extern "C" void HAL_getCameraInfo(int cameraId, struct CameraInfo* cameraInfo);

int qcamera_device_open(const hw_module_t* module, const char* name,
                        hw_device_t** device);
int CameraHAL_GetCam_Info(int camera_id, struct camera_info *info);

/* Global variables. */
camera_notify_callback         origNotify_cb    = NULL;
camera_data_callback           origData_cb      = NULL;
camera_data_timestamp_callback origDataTS_cb    = NULL;
camera_request_memory          origCamReqMemory = NULL;
bool			       externallyRequestedFrames = false;
android::Vector<camera_memory_t*>       sentFrames;

android::String8          g_str;
android::CameraParameters camSettings;
preview_stream_ops_t      *mWindow = NULL;
android::sp<android::CameraHardwareInterface> qCamera;

static hw_module_methods_t camera_module_methods = {
   open: qcamera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
   common: {
      tag: HARDWARE_MODULE_TAG,
      version_major: 1,
      version_minor: 0,
      id: CAMERA_HARDWARE_MODULE_ID,
      name: "Camera HAL for ICS",
      author: "Raviprasad V Mummidi",
      methods: &camera_module_methods,
      dso: NULL,
      reserved: {0},
   },
   get_number_of_cameras: HAL_getNumberOfCameras,
   get_camera_info: CameraHAL_GetCam_Info,
};		

int
CameraHAL_GetCam_Info(int camera_id, struct camera_info *info)
{
   LOGV("CameraHAL_GetCam_Info:\n");
   HAL_getCameraInfo(camera_id, (struct CameraInfo*) info);
   /* Disregard that... */
   info->facing      = CAMERA_FACING_BACK;
   info->orientation = 90;
   return NO_ERROR;
}

/* HAL helper functions. */
void 
CameraHAL_NotifyCb(int32_t msg_type, int32_t ext1,
                   int32_t ext2, void *user)
{
   LOGV("CameraHAL_NotifyCb: msg_type:%d ext1:%d ext2:%d user:%p\n",
        msg_type, ext1, ext2, user);
   if (origNotify_cb != NULL) {
      origNotify_cb(msg_type, ext1, ext2, user);
   }
}

bool
CameraHAL_CopyBuffers_Hw(int srcFd, int destFd,
                         size_t srcOffset, size_t destOffset,
                         int srcFormat, int destFormat,
                         int x, int y, int w, int h)
{
    struct blitreq blit;
    bool   success = true;
    int    fb_fd = open("/dev/graphics/fb0", O_RDWR);

    if (fb_fd < 0) {
       LOGD("CameraHAL_CopyBuffers_Hw: Error opening /dev/graphics/fb0\n");
       return false;
    }

    LOGV("CameraHAL_CopyBuffers_Hw: srcFD:%d destFD:%d srcOffset:%#x"
         " destOffset:%#x x:%d y:%d w:%d h:%d\n", srcFd, destFd, srcOffset,
         destOffset, x, y, w, h);

    memset(&blit, 0, sizeof(blit));
    blit.count = 1;

    blit.req.flags       = 0;
    blit.req.alpha       = 0xff;
    blit.req.transp_mask = 0xffffffff;

    blit.req.src.width     = w;
    blit.req.src.height    = h;
    blit.req.src.offset    = srcOffset;
    blit.req.src.memory_id = srcFd;
    blit.req.src.format    = srcFormat;

    blit.req.dst.width     = w;
    blit.req.dst.height    = h;
    blit.req.dst.offset    = destOffset;
    blit.req.dst.memory_id = destFd;
    blit.req.dst.format    = destFormat;

    blit.req.src_rect.x = blit.req.dst_rect.x = x;
    blit.req.src_rect.y = blit.req.dst_rect.y = y;
    blit.req.src_rect.w = blit.req.dst_rect.w = w;
    blit.req.src_rect.h = blit.req.dst_rect.h = h;

    if (ioctl(fb_fd, MSMFB_BLIT, &blit)) {
       LOGV("CameraHAL_CopyBuffers_Hw: MSMFB_BLIT failed = %d %s\n",
            errno, strerror(errno));
       success = false;
    }
    close(fb_fd);
    return success;
}

void
CameraHAL_HandlePreviewData(const android::sp<android::IMemory>& dataPtr,
                            preview_stream_ops_t *mWindow,
                            camera_request_memory getMemory,
                            int32_t previewWidth, int32_t previewHeight)
{
   if (mWindow != NULL && getMemory != NULL) {
      ssize_t  offset;
      size_t   size;
      int32_t  previewFormat = MDP_Y_CBCR_H2V2;
      int32_t  destFormat    = MDP_BGRA_8888;

      android::status_t retVal;
      android::sp<android::IMemoryHeap> mHeap = dataPtr->getMemory(&offset,
                                                                   &size);

      LOGV("CameraHAL_HandlePreviewData: previewWidth:%d previewHeight:%d "
           "offset:%#x size:%#x base:%p\n", previewWidth, previewHeight,
           (unsigned)offset, size, mHeap != NULL ? mHeap->base() : 0);

      mWindow->set_usage(mWindow, GRALLOC_USAGE_PRIVATE_0 |
                         GRALLOC_USAGE_SW_READ_OFTEN);
      retVal = mWindow->set_buffers_geometry(mWindow,
                                             previewWidth, previewHeight,
                                             HAL_PIXEL_FORMAT_RGBX_8888
                                             );
      if (retVal == NO_ERROR) {
         int32_t          stride;
         buffer_handle_t *bufHandle = NULL;

         LOGV("CameraHAL_HandlePreviewData: dequeueing buffer\n");
         retVal = mWindow->dequeue_buffer(mWindow, &bufHandle, &stride);
         if (retVal == NO_ERROR) {
            retVal = mWindow->lock_buffer(mWindow, bufHandle);
            if (retVal == NO_ERROR) {
               private_handle_t const *privHandle =
                  reinterpret_cast<private_handle_t const *>(*bufHandle);
               CameraHAL_CopyBuffers_Hw(mHeap->getHeapID(), privHandle->fd,
                                             offset, privHandle->offset,
                                             previewFormat, destFormat,
                                             0, 0, previewWidth,
                                             previewHeight);
               mWindow->enqueue_buffer(mWindow, bufHandle);
               LOGV("CameraHAL_HandlePreviewData: enqueued buffer\n");
            } else {
               LOGE("CameraHAL_HandlePreviewData: ERROR locking the buffer\n");
               mWindow->cancel_buffer(mWindow, bufHandle);
            }
         } else {
            LOGE("CameraHAL_HandlePreviewData: ERROR dequeueing the buffer\n");
         }
      }
   }
}

typedef struct {
uint32_t dx;
uint32_t dy;
unsigned char * imgPtr;
} yuv_image_type;

int (*LINK_yuv_convert_ycrcb420sp_to_yv12_inplace) (yuv_image_type* yuvStructPtr);

camera_memory_t *
CameraHAL_GenClientData(const android::sp<android::IMemory> &dataPtr, 
                        camera_request_memory reqClientMemory,
                        void *user)
{
   ssize_t          offset;
   size_t           size;
   camera_memory_t *clientData = NULL;

      /*int32_t previewWidth, previewHeight;
      android::CameraParameters hwParameters = qCamera->getParameters();
      hwParameters.getPreviewSize(&previewWidth, &previewHeight);
	yuv_image_type in_buf;
               in_buf.imgPtr = (unsigned char *)dataPtr->pointer();
               in_buf.dx  = previewWidth;
               in_buf.dy  = previewHeight;
	LOGD("DOING INLINE CONVERSION for %d", previewWidth);
      LINK_yuv_convert_ycrcb420sp_to_yv12_inplace(&in_buf);*/

   android::sp<android::IMemoryHeap> mHeap = dataPtr->getMemory(&offset, &size);

   LOGV("CameraHAL_GenClientData: offset:%#x size:%#x base:%p\n",
        (unsigned)offset, size, mHeap != NULL ? mHeap->base() : 0);

   clientData = reqClientMemory(-1, size, 1, user);
   if (clientData != NULL) {

      //clientData->data = (char *)(mHeap->base()) + offset;

      memcpy(clientData->data, (char *)(mHeap->base()) + offset, size);
   } else {
      LOGV("CameraHAL_GenClientData: ERROR allocating memory from client\n");
   }
   return clientData;
}

void 
CameraHAL_DataCb(int32_t msg_type, const android::sp<android::IMemory>& dataPtr,
                 void *user)
{
   LOGV("CameraHAL_DataCb: msg_type:%d user:%p\n", msg_type, user);

   if ((msg_type != CAMERA_MSG_PREVIEW_FRAME || externallyRequestedFrames) && 
          origData_cb != NULL && origCamReqMemory != NULL) {
      camera_memory_t *clientData = CameraHAL_GenClientData(dataPtr,
                                       origCamReqMemory, user);
      if (clientData != NULL) {
         LOGV("CameraHAL_DataCb: Posting %d data to client\n",msg_type);
         origData_cb(msg_type, clientData, 0, NULL, user);
         clientData->release(clientData);
      }
   }
   if (msg_type == CAMERA_MSG_PREVIEW_FRAME) {
      int32_t previewWidth, previewHeight;
      android::CameraParameters hwParameters = qCamera->getParameters();
      hwParameters.getPreviewSize(&previewWidth, &previewHeight);
      CameraHAL_HandlePreviewData(dataPtr, mWindow, origCamReqMemory,
                                  previewWidth, previewHeight);
   } 
}

void 
CameraHAL_DataTSCb(nsecs_t timestamp, int32_t msg_type,
                   const android::sp<android::IMemory>& dataPtr, void *user)
{
   LOGD("CameraHAL_DataTSCb: timestamp:%lld now:%lld msg_type:%d user:%p\n",
        timestamp /1000, systemTime(), msg_type, user);

   /*if (externallyRequestedFrames && origData_cb != NULL && 
          origCamReqMemory != NULL) {
      camera_memory_t *clientData = CameraHAL_GenClientData(dataPtr,
                                       origCamReqMemory, user);
      if (clientData != NULL) {
         LOGV("CameraHAL_DataCb: Posting %d data to client\n",msg_type);
         origData_cb(msg_type, clientData, 0, NULL, user);
         clientData->release(clientData);
      }
   }*/

   if (origDataTS_cb != NULL && origCamReqMemory != NULL) {
      camera_memory_t *clientData = CameraHAL_GenClientData(dataPtr,
                                       origCamReqMemory, user);
      if (clientData != NULL) {
         LOGV("CameraHAL_DataTSCb: Posting data to client timestamp:%lld\n", 
              systemTime());

	 sentFrames.push_back(clientData);
         origDataTS_cb(timestamp, msg_type, sentFrames.top(), 0, user);
         qCamera->releaseRecordingFrame(dataPtr);
      } else {
         LOGD("CameraHAL_DataTSCb: ERROR allocating memory from client\n");
      }
   }
}

void
CameraHAL_FixupParams(android::CameraParameters &settings)
{
   const char *preview_sizes =
      "640x480,576x432,480x320,384x288,352x288,320x240,240x160,176x144";
   const char *video_sizes = 
      "640x480,352x288,320x240,176x144";
   const char *preferred_size       = "640x480";
   const char *preview_frame_rates  = "30,27,24,15";
   const char *preferred_frame_rate = "15";

   settings.set(android::CameraParameters::KEY_VIDEO_FRAME_FORMAT,
                android::CameraParameters::PIXEL_FORMAT_YV12);

   settings.set(android::CameraParameters::KEY_PREVIEW_FORMAT,
                android::CameraParameters::PIXEL_FORMAT_NV12);

   if (!settings.get(android::CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES)) {
      settings.set(android::CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES,
                   preview_sizes);
   }

   if (!settings.get(android::CameraParameters::KEY_SUPPORTED_VIDEO_SIZES)) {
      settings.set(android::CameraParameters::KEY_SUPPORTED_VIDEO_SIZES,
                   video_sizes);
   }

   if (!settings.get(android::CameraParameters::KEY_VIDEO_SIZE)) {
      settings.set(android::CameraParameters::KEY_VIDEO_SIZE, preferred_size);
   }

   if (!settings.get(android::CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO)) {
      settings.set(android::CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO,
                   preferred_size);
   }

   if (!settings.get(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES)) {
      settings.set(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,
                   preview_frame_rates);
   }

   if (!settings.get(android::CameraParameters::KEY_PREVIEW_FRAME_RATE)) {
      settings.set(android::CameraParameters::KEY_PREVIEW_FRAME_RATE,
                   preferred_frame_rate);
   }
}

static void releaseCameraFrames()
{
    android::Vector<camera_memory_t*>::iterator it;
    for (it = sentFrames.begin(); it != sentFrames.end(); ++it) {
        (*it)->release(*it);
    }
    sentFrames.clear();
}

/* Hardware Camera interface handlers. */
int 
qcamera_set_preview_window(struct camera_device * device, 
                           struct preview_stream_ops *window)
{
   LOGV("qcamera_set_preview_window : Window :%p\n", window);
   if (device == NULL) {
      LOGE("qcamera_set_preview_window : Invalid device.\n");
      return -EINVAL;
   } else {
      LOGV("qcamera_set_preview_window : window :%p\n", window);
      mWindow = window;
      return 0;
   }
}

void 
qcamera_set_callbacks(struct camera_device * device, 
                      camera_notify_callback notify_cb,    
                      camera_data_callback data_cb,       
                      camera_data_timestamp_callback data_cb_timestamp,        
                      camera_request_memory get_memory, void *user)
{
   LOGV("qcamera_set_callbacks: notify_cb: %p, data_cb: %p "
        "data_cb_timestamp: %p, get_memory: %p, user :%p", 
        notify_cb, data_cb, data_cb_timestamp, get_memory, user);

   origNotify_cb    = notify_cb;
   origData_cb      = data_cb;
   origDataTS_cb    = data_cb_timestamp;
   origCamReqMemory = get_memory;
   qCamera->setCallbacks(CameraHAL_NotifyCb, CameraHAL_DataCb,
                         CameraHAL_DataTSCb, user);
}

void 
qcamera_enable_msg_type(struct camera_device * device, int32_t msg_type)
{
   if (msg_type & CAMERA_MSG_PREVIEW_FRAME)
       externallyRequestedFrames = true;

   qCamera->enableMsgType(msg_type);
}

void 
qcamera_disable_msg_type(struct camera_device * device, int32_t msg_type)
{
   if (msg_type & CAMERA_MSG_PREVIEW_FRAME)
       externallyRequestedFrames = false;
   LOGV("qcamera_disable_msg_type: msg_type:%d\n", msg_type);
   if (msg_type == CAMERA_MSG_VIDEO_FRAME) {
       LOGW("%s: releasing stale video frames", __FUNCTION__);
       releaseCameraFrames();
   }
   qCamera->disableMsgType(msg_type);
}

int 
qcamera_msg_type_enabled(struct camera_device * device, int32_t msg_type)
{
   LOGV("qcamera_msg_type_enabled: msg_type:%d\n", msg_type);
   return qCamera->msgTypeEnabled(msg_type);
}

int 
qcamera_start_preview(struct camera_device * device)
{
   LOGV("qcamera_start_preview: Enabling CAMERA_MSG_PREVIEW_FRAME\n");

   /* TODO: Remove hack. */
   qCamera->enableMsgType(CAMERA_MSG_PREVIEW_FRAME);
   return qCamera->startPreview();
}

void 
qcamera_stop_preview(struct camera_device * device)
{
   LOGV("qcamera_stop_preview:\n");

   /* TODO: Remove hack. */
   qCamera->disableMsgType(CAMERA_MSG_PREVIEW_FRAME);
   return qCamera->stopPreview();
}

int 
qcamera_preview_enabled(struct camera_device * device)
{
   LOGV("qcamera_preview_enabled:\n");
   return qCamera->previewEnabled() ? 1 : 0;
}

int 
qcamera_store_meta_data_in_buffers(struct camera_device * device, int enable)
{
   LOGV("qcamera_store_meta_data_in_buffers:\n");
   return -1;
}

int 
qcamera_start_recording(struct camera_device * device)
{
   LOGV("qcamera_start_recording\n");

   /* TODO: Remove hack. */
   qCamera->enableMsgType(CAMERA_MSG_VIDEO_FRAME);
   qCamera->startRecording();
   return NO_ERROR;
}

void 
qcamera_stop_recording(struct camera_device * device)
{
   LOGV("qcamera_stop_recording:\n");

   /* TODO: Remove hack. */
   qCamera->disableMsgType(CAMERA_MSG_VIDEO_FRAME);
   qCamera->stopRecording();
}

int 
qcamera_recording_enabled(struct camera_device * device)
{
   LOGV("qcamera_recording_enabled:\n");
   return (int)qCamera->recordingEnabled();
}

void 
qcamera_release_recording_frame(struct camera_device * device, 
                                const void *opaque)
{
   LOGV("qcamera_release_recording_frame: opaque:%p\n", opaque);
   if (opaque != NULL) {
       android::Vector<camera_memory_t*>::iterator it;
       for (it = sentFrames.begin(); it != sentFrames.end(); ++it) {
           camera_memory_t *mem = *it;
           if (mem->data == opaque) {
               mem->release(mem);
               sentFrames.erase(it);
               break;
           }
       }
   }
}

int 
qcamera_auto_focus(struct camera_device * device)
{
   LOGV("qcamera_auto_focus:\n");
   qCamera->autoFocus();
   return NO_ERROR;
}

int 
qcamera_cancel_auto_focus(struct camera_device * device)
{
   LOGV("qcamera_cancel_auto_focus:\n");
   qCamera->cancelAutoFocus();
   return NO_ERROR;
}

int 
qcamera_take_picture(struct camera_device * device)
{
   LOGV("qcamera_take_picture:\n");

   /* TODO: Remove hack. */
   qCamera->enableMsgType(CAMERA_MSG_SHUTTER |
                         CAMERA_MSG_POSTVIEW_FRAME |
                         CAMERA_MSG_RAW_IMAGE |
                         CAMERA_MSG_COMPRESSED_IMAGE);

   qCamera->takePicture();
   return NO_ERROR;
}

int 
qcamera_cancel_picture(struct camera_device * device)
{
   LOGV("camera_cancel_picture:\n");
   qCamera->cancelPicture();
   return NO_ERROR;	
}

int 
qcamera_set_parameters(struct camera_device * device, const char *params)
{
   LOGV("qcamera_set_parameters: %s\n", params);
   g_str = android::String8(params);
   camSettings.unflatten(g_str);
   qCamera->setParameters(camSettings);
   return NO_ERROR;
}

char* 
qcamera_get_parameters(struct camera_device * device)
{ 
   char *rc = NULL;
   LOGV("qcamera_get_parameters\n");
   camSettings = qCamera->getParameters();
   LOGV("qcamera_get_parameters: after calling qCamera->getParameters()\n");
   CameraHAL_FixupParams(camSettings);
   g_str = camSettings.flatten();
   rc = strdup((char *)g_str.string());
   LOGV("camera_get_parameters: returning rc:%p :%s\n", 
        rc, (rc != NULL) ? rc : "EMPTY STRING");
   return rc;
}

void 
qcamera_put_parameters(struct camera_device *device, char *params)
{
   LOGV("qcamera_put_parameters: params:%p %s", params, params);
   free(params);
}


int 
qcamera_send_command(struct camera_device * device, int32_t cmd, 
                        int32_t arg0, int32_t arg1)
{
   LOGV("qcamera_send_command: cmd:%d arg0:%d arg1:%d\n", 
        cmd, arg0, arg1);
   return qCamera->sendCommand(cmd, arg0, arg1);
}

void 
qcamera_release(struct camera_device * device)
{
   LOGV("camera_release:\n");
   releaseCameraFrames();
   qCamera->release();
}

int 
qcamera_dump(struct camera_device * device, int fd)
{
   LOGV("qcamera_dump:\n");
   android::Vector<android::String16> args;
   return qCamera->dump(fd, args);
}

int 
camera_device_close(hw_device_t* device)
{	
   int rc = -EINVAL;
   LOGD("camera_device_close\n");
   camera_device_t *cameraDev = (camera_device_t *)device;
   if (cameraDev) {
      camera_device_ops_t *camera_ops = cameraDev->ops;
      if (camera_ops) {
         if (qCamera != NULL) {
            qCamera.clear();
         }
         free(camera_ops);
      }
      free(cameraDev);
      rc = NO_ERROR;
   }
   return rc;
}

int 
qcamera_device_open(const hw_module_t* module, const char* name, 
                   hw_device_t** device)
{

   int cameraId = atoi(name);

   LOGD("qcamera_device_open: name:%s device:%p cameraId:%d\n", 
        name, device, cameraId);

   qCamera = HAL_openCameraHardware(cameraId, 5);
   camera_device_t* camera_device = NULL;
   camera_device_ops_t* camera_ops = NULL;

   camera_device = (camera_device_t*)malloc(sizeof(*camera_device));
   camera_ops = (camera_device_ops_t*)malloc(sizeof(*camera_ops));
   memset(camera_device, 0, sizeof(*camera_device));
   memset(camera_ops, 0, sizeof(*camera_ops));

   camera_device->common.tag              = HARDWARE_DEVICE_TAG;
   camera_device->common.version          = 0;
   camera_device->common.module           = (hw_module_t *)(module);
   camera_device->common.close            = camera_device_close;
   camera_device->ops                     = camera_ops;	
           
   camera_ops->set_preview_window         = qcamera_set_preview_window;
   camera_ops->set_callbacks              = qcamera_set_callbacks;
   camera_ops->enable_msg_type            = qcamera_enable_msg_type;
   camera_ops->disable_msg_type           = qcamera_disable_msg_type;
   camera_ops->msg_type_enabled           = qcamera_msg_type_enabled;
   camera_ops->start_preview              = qcamera_start_preview;
   camera_ops->stop_preview               = qcamera_stop_preview;
   camera_ops->preview_enabled            = qcamera_preview_enabled;
   camera_ops->store_meta_data_in_buffers = qcamera_store_meta_data_in_buffers;
   camera_ops->start_recording            = qcamera_start_recording;
   camera_ops->stop_recording             = qcamera_stop_recording;
   camera_ops->recording_enabled          = qcamera_recording_enabled;
   camera_ops->release_recording_frame    = qcamera_release_recording_frame;
   camera_ops->auto_focus                 = qcamera_auto_focus;
   camera_ops->cancel_auto_focus          = qcamera_cancel_auto_focus;
   camera_ops->take_picture               = qcamera_take_picture;
   camera_ops->cancel_picture             = qcamera_cancel_picture;

   camera_ops->set_parameters             = qcamera_set_parameters;
   camera_ops->get_parameters             = qcamera_get_parameters;
   camera_ops->put_parameters             = qcamera_put_parameters;
   camera_ops->send_command               = qcamera_send_command;
   camera_ops->release                    = qcamera_release;
   camera_ops->dump                       = qcamera_dump;

   *device = &camera_device->common;


void *libmmcamera = ::dlopen("liboemcamera.so", RTLD_NOW);
   if (!libmmcamera) {
       LOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
      }

*(void **)&LINK_yuv_convert_ycrcb420sp_to_yv12_inplace =
        ::dlsym(libmmcamera, "yuv_convert_ycrcb420sp_to_yv12");
   return NO_ERROR;
}