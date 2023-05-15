#include <SDL.h>
#include <stdio.h>
#include <unistd.h>

#include "libuvc/libuvc.h"

int main(int argc, char **argv) {
  uvc_context_t *ctx;
  uvc_device_t *dev;
  uvc_device_handle_t *devh;
  uvc_stream_ctrl_t ctrl;
  uvc_error_t res;
  SDL_Window *window = NULL;
  SDL_Renderer *sdlRenderer = NULL;
  SDL_Texture *sdlTexture = NULL;
  SDL_Rect sdlRect;
  const int screen_w = 960;
  const int screen_h = 540;
  sdlRect.x = 0;
  sdlRect.y = 0;
  sdlRect.w = screen_w;
  sdlRect.h = screen_h;

  SDL_Init(SDL_INIT_VIDEO);
  // Create sdl window
  window = SDL_CreateWindow("Simplest Video Play SDL2", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, screen_w, screen_h,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

  sdlRenderer = SDL_CreateRenderer(window, -1, 0);

  /* Initialize a UVC service context. Libuvc will set up its own libusb
   * context. Replace NULL with a libusb_context pointer to run libuvc
   * from an existing libusb context. */
  res = uvc_init(&ctx, NULL);

  if (res < 0) {
    uvc_perror(res, "uvc_init");
    return res;
  }

  puts("UVC initialized");

  /* Locates the first attached UVC device, stores in dev */
  res = uvc_find_device(
      ctx, &dev, 0, 0,
      NULL); /* filter devices: vendor_id, product_id, "serial_num" */

  if (res < 0) {
    uvc_perror(res, "uvc_find_device"); /* no devices found */
  } else {
    puts("Device found");

    /* Try to open the device: requires exclusive access */
    res = uvc_open(dev, &devh);

    if (res < 0) {
      uvc_perror(res, "uvc_open"); /* unable to open device */
    } else {
      puts("Device opened");

      /* Print out a message containing all the information that libuvc
       * knows about the device */
      uvc_print_diag(devh, stderr);

      const uvc_format_desc_t *format_desc = uvc_get_format_descs(devh)->next;
      const uvc_frame_desc_t *frame_desc = format_desc->frame_descs->next;

      enum uvc_frame_format frame_format;
      int width = 640;
      int height = 480;
      int fps = 30;
      Uint32 pixformat = SDL_PIXELFORMAT_IYUV;

      switch (format_desc->bDescriptorSubtype) {
        case UVC_VS_FORMAT_MJPEG:
          frame_format = UVC_COLOR_FORMAT_MJPEG;
          break;
        case UVC_VS_FORMAT_FRAME_BASED:
          frame_format = UVC_FRAME_FORMAT_H264;
          break;
        default:
          frame_format = UVC_FRAME_FORMAT_NV12;
          pixformat = SDL_PIXELFORMAT_NV12;
          break;
      }

      if (frame_desc) {
        width = frame_desc->wWidth;
        height = frame_desc->wHeight;
        fps = 10000000 / frame_desc->dwDefaultFrameInterval;
      }

      sdlTexture = SDL_CreateTexture(
          sdlRenderer, pixformat, SDL_TEXTUREACCESS_STREAMING, width, height);

      printf("\nFirst format: (%4s) %dx%d %dfps\n", format_desc->fourccFormat,
             width, height, fps);

      /* Try to negotiate first stream profile */
      res = uvc_get_stream_ctrl_format_size(
          devh, &ctrl,                     /* result stored in ctrl */
          frame_format, width, height, fps /* width, height, fps */
      );

      /* Print out the result */
      uvc_print_stream_ctrl(&ctrl, stderr);

      if (res < 0) {
        uvc_perror(res,
                   "get_mode"); /* device doesn't provide a matching stream */
      } else {
        uvc_stream_handle_t *strmh;

        res = uvc_stream_open_ctrl(devh, &strmh, &ctrl);
        if (res < 0) {
          uvc_perror(res, "uvc_stream_open_ctrl");
        } else {
          res = uvc_stream_start(strmh, NULL, NULL, 0);

          if (res < 0) {
            uvc_stream_close(strmh);
            uvc_perror(res, "start_streaming"); /* unable to start stream */
          } else {
            puts("Streaming...");

            /* enable auto exposure - see uvc_set_ae_mode documentation */
            puts("Enabling auto exposure ...");
            // uvc_set_focus_auto(devh, 0);
            // uvc_set_focus_abs(devh, 1);
            const uint8_t UVC_AUTO_EXPOSURE_MODE_AUTO = 2;
            res = uvc_set_ae_mode(devh, UVC_AUTO_EXPOSURE_MODE_AUTO);
            if (res == UVC_SUCCESS) {
              puts(" ... enabled auto exposure");
            } else if (res == UVC_ERROR_PIPE) {
              /* this error indicates that the camera does not support the full
               * AE mode; try again, using aperture priority mode (fixed
               * aperture, variable exposure time) */
              puts(" ... full AE not supported, trying aperture priority mode");
              const uint8_t UVC_AUTO_EXPOSURE_MODE_APERTURE_PRIORITY = 8;
              res = uvc_set_ae_mode(devh,
                                    UVC_AUTO_EXPOSURE_MODE_APERTURE_PRIORITY);
              if (res < 0) {
                uvc_perror(res,
                           " ... uvc_set_ae_mode failed to enable aperture "
                           "priority mode");
              } else {
                puts(" ... enabled aperture priority auto exposure mode");
              }
            } else {
              uvc_perror(
                  res,
                  " ... uvc_set_ae_mode failed to enable auto exposure mode");
            }

            SDL_Event event;
            SDL_bool done = SDL_FALSE;
            Uint32 windowID = SDL_GetWindowID(window);
            while (1) {
              if (SDL_PollEvent(&event)) {
                switch (event.type) {
                  case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                      done = SDL_TRUE;
                    }
                    break;
                  case SDL_QUIT:
                    done = SDL_TRUE;
                  case SDL_WINDOWEVENT:
                    if (event.window.windowID == windowID &&
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                      sdlRect.w = event.window.data1;
                      sdlRect.h = event.window.data2;
                      printf("Resize window. Width=%d, height=%d\n", sdlRect.w,
                             sdlRect.h);
                    }
                    break;
                }
              }

              if (done) break;

              uvc_frame_t *frame;
              res = uvc_stream_get_frame(strmh, &frame, 0);

              if (res < 0) {
                printf("unable to get a stream frame!\n");
                continue;
              }

              // printf(
              //     "uvc get frame! frame_format = %d, width = %d, height = %d,
              //     " "length = %lu, sequence=%d\n", frame->frame_format,
              //     frame->width, frame->height, frame->data_bytes,
              //     frame->sequence);
              switch (frame->frame_format) {
                case UVC_FRAME_FORMAT_H264:
                  break;
                case UVC_COLOR_FORMAT_MJPEG:
                  break;
                case UVC_COLOR_FORMAT_YUYV:
                  break;
                case UVC_COLOR_FORMAT_NV12:
                  SDL_UpdateNVTexture(
                      sdlTexture, NULL, frame->data, frame->width,
                      frame->data + (frame->height * frame->width),
                      frame->width);
                  SDL_RenderClear(sdlRenderer);
                  SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
                  SDL_RenderPresent(sdlRenderer);
                  break;
                default:
                  break;
              }
            }

            /* End the stream. Blocks until last callback is serviced */
            uvc_stop_streaming(devh);
            puts("Done streaming.");
          }
        }
      }

      SDL_DestroyTexture(sdlTexture);
      /* Release our handle on the device */
      uvc_close(devh);
      puts("Device closed");
    }

    /* Release the device descriptor */
    uvc_unref_device(dev);
  }

  /* Close the UVC context. This closes and cleans up any existing device
   * handles, and it closes the libusb context if one was not provided. */
  uvc_exit(ctx);
  puts("UVC exited");
  SDL_DestroyRenderer(sdlRenderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
