/**
 * Copyright (c) 2021 totsucom
 * This software is released under the MIT License.
 * http://opensource.org/licenses/MIT
 *
 * jpeg品質パラメータや回転パラメータを追加
 * オリジナルの製作者様は下記の通り。感謝！
 */

/**
 * @file jpeg.c
 *
 * Copyright (c) 2015 大前良介 (OHMAE Ryosuke)
 *
 * This software is released under the MIT License.
 * http://opensource.org/licenses/MIT
 *
 * @brief JPEGファイルの読み書き処理
 * @author <a href="mailto:ryo@mm2d.net">大前良介 (OHMAE Ryosuke)</a>
 * @date 2015/02/07
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <jpeglib.h>
#include "image.h"
#include <setjmp.h>

/**
 * jpeg_error_mgrの拡張。
 */
typedef struct my_error_mgr {
  struct jpeg_error_mgr jerr;
  jmp_buf jmpbuf;
} my_error_mgr;

/**
 * 致命的エラー発生時の処理。
 */
static void error_exit(j_common_ptr cinfo) {
  my_error_mgr *err = (my_error_mgr *) cinfo->err;
  (*cinfo->err->output_message)(cinfo);
  longjmp(err->jmpbuf, 1);
}

/**
 * @brief JPEG形式のファイルを読み込む。
 *
 * @param[in] filename ファイル名
 * @return 読み込んだ画像、読み込みに失敗した場合NULL
 */
image_t *read_jpeg_file(const char *filename) {
  FILE *fp;
  if ((fp = fopen(filename, "rb")) == NULL) {
    perror(filename);
    return NULL;
  }
  image_t *img = read_jpeg_stream(fp);
  fclose(fp);
  return img;
}

/**
 * @brief JPEG形式のファイルを読み込む。
 *
 * @param[in] fp ファイルストリーム
 * @return 読み込んだ画像、読み込みに失敗した場合NULL
 */
image_t *read_jpeg_stream(FILE *fp) {
  result_t result = FAILURE;
  uint32_t x, y;
  struct jpeg_decompress_struct jpegd;
  my_error_mgr myerr;
  image_t *img = NULL;
  JSAMPROW buffer = NULL;
  JSAMPROW row;
  int stride;
  jpegd.err = jpeg_std_error(&myerr.jerr);
  myerr.jerr.error_exit = error_exit;
  if (setjmp(myerr.jmpbuf)) {
    goto error;
  }
  jpeg_create_decompress(&jpegd);
  jpeg_stdio_src(&jpegd, fp);
  if (jpeg_read_header(&jpegd, TRUE) != JPEG_HEADER_OK) {
    goto error;
  }
  jpeg_start_decompress(&jpegd);
  if (jpegd.out_color_space != JCS_RGB) {
    goto error;
  }
  stride = sizeof(JSAMPLE) * jpegd.output_width * jpegd.output_components;
  if ((buffer = calloc(stride, 1)) == NULL) {
    goto error;
  }
  if ((img = allocate_image(jpegd.output_width, jpegd.output_height,
                            COLOR_TYPE_RGB)) == NULL) {
    goto error;
  }
  for (y = 0; y < jpegd.output_height; y++) {
    jpeg_read_scanlines(&jpegd, &buffer, 1);
    row = buffer;
    for (x = 0; x < jpegd.output_width; x++) {
      img->map[y][x].c.r = *row++;
      img->map[y][x].c.g = *row++;
      img->map[y][x].c.b = *row++;
      img->map[y][x].c.a = 0xff;
    }
  }
  jpeg_finish_decompress(&jpegd);
  result = SUCCESS;
  error:
  jpeg_destroy_decompress(&jpegd);
  free(buffer);
  if (result != SUCCESS) {
    free_image(img);
    img = NULL;
  }
  return img;
}

//by totsucom
//読み込み時に回転できる
image_t *read_jpeg_streamEx(FILE *fp, int rotation) {
  switch (rotation) {
    case ROTATE_CLOCKWISE_0:   break;
    case ROTATE_CLOCKWISE_90:  break;
    case ROTATE_CLOCKWISE_180: break;
    case ROTATE_CLOCKWISE_270: break;
    default: return NULL;
  }
  result_t result = FAILURE;
  int32_t x, y, create_w, create_h; //for文ように符号が必要
  struct jpeg_decompress_struct jpegd;
  my_error_mgr myerr;
  image_t *img = NULL;
  JSAMPROW buffer = NULL;
  JSAMPROW row;
  int stride;
  jpegd.err = jpeg_std_error(&myerr.jerr);
  myerr.jerr.error_exit = error_exit;
  if (setjmp(myerr.jmpbuf)) {
    goto error;
  }
  jpeg_create_decompress(&jpegd);
  jpeg_stdio_src(&jpegd, fp);
  if (jpeg_read_header(&jpegd, TRUE) != JPEG_HEADER_OK) {
    goto error;
  }
  jpeg_start_decompress(&jpegd);
  if (jpegd.out_color_space != JCS_RGB) {
    goto error;
  }
  stride = sizeof(JSAMPLE) * jpegd.output_width * jpegd.output_components;
  if ((buffer = calloc(stride, 1)) == NULL) {
    goto error;
  }
  if (rotation == ROTATE_CLOCKWISE_0 || rotation == ROTATE_CLOCKWISE_180) {
    create_w = jpegd.output_width;
    create_h = jpegd.output_height;
  } else {
    create_w = jpegd.output_height;
    create_h = jpegd.output_width;
  }
  if ((img = allocate_image(create_w, create_h, COLOR_TYPE_RGB)) == NULL) {
    goto error;
  }
  switch (rotation) {
    case ROTATE_CLOCKWISE_0:
      for (y = 0; y < jpegd.output_height; ++y) {
        jpeg_read_scanlines(&jpegd, &buffer, 1);
        row = buffer;
        for (x = 0; x < jpegd.output_width; ++x) {
          img->map[y][x].c.r = *row++;
          img->map[y][x].c.g = *row++;
          img->map[y][x].c.b = *row++;
          img->map[y][x].c.a = 0xff;
        }
      }
      break;
    case ROTATE_CLOCKWISE_180:
      for (y = jpegd.output_height - 1; y >= 0; --y) {
        jpeg_read_scanlines(&jpegd, &buffer, 1);
        row = buffer;
        for (x = jpegd.output_width - 1; x >= 0; --x) {
          img->map[y][x].c.r = *row++;
          img->map[y][x].c.g = *row++;
          img->map[y][x].c.b = *row++;
          img->map[y][x].c.a = 0xff;
        }
      }
      break;
    case ROTATE_CLOCKWISE_90:
      for (y = jpegd.output_height - 1; y >= 0; --y) {
        jpeg_read_scanlines(&jpegd, &buffer, 1);
        row = buffer;
        for (x = 0; x < jpegd.output_width; ++x) {
          img->map[x][y].c.r = *row++;
          img->map[x][y].c.g = *row++;
          img->map[x][y].c.b = *row++;
          img->map[x][y].c.a = 0xff;
        }
      }
      break;
    default: //ROTATE_CLOCKWISE_270:
      for (y = 0; y < jpegd.output_height; ++y) {
        jpeg_read_scanlines(&jpegd, &buffer, 1);
        row = buffer;
        for (x = jpegd.output_width - 1; x >= 0; --x) {
          img->map[x][y].c.r = *row++;
          img->map[x][y].c.g = *row++;
          img->map[x][y].c.b = *row++;
          img->map[x][y].c.a = 0xff;
        }
      }
  }
  jpeg_finish_decompress(&jpegd);
  result = SUCCESS;
  error:
  jpeg_destroy_decompress(&jpegd);
  free(buffer);
  if (result != SUCCESS) {
    free_image(img);
    img = NULL;
  }
  return img;
}

/**
 * @brief JPEG形式としてファイルに書き出す。
 *
 * @param[in] filename 書き出すファイル名
 * @param[in] img      画像データ
 * @param[in] quality 品質 0-100(75)
 * @return 成否
 */
result_t write_jpeg_file(const char *filename, image_t *img, int quality) {
  result_t result = FAILURE;
  FILE *fp;
  if (img == NULL) {
    return result;
  }
  if ((fp = fopen(filename, "wb")) == NULL) {
    perror(filename);
    return result;
  }
  result = write_jpeg_stream(fp, img, quality);
  fclose(fp);
  return result;
}

/**
 * @brief JPEG形式としてファイルに書き出す。
 *
 * @param[in] fp  書き出すファイルストリームのポインタ
 * @param[in] img 画像データ
 * @param[in] quality 品質 0-100(75)
 * @return 成否
 */
result_t write_jpeg_stream(FILE *fp, image_t *img, int quality) {
  result_t result = FAILURE;
  int x, y;
  struct jpeg_compress_struct jpegc;
  my_error_mgr myerr;
  image_t *to_free = NULL;
  JSAMPROW buffer = NULL;
  JSAMPROW row;
  if (img == NULL) {
    return FAILURE;
  }
  if ((buffer = malloc(sizeof(JSAMPLE) * 3 * img->width)) == NULL) {
    return FAILURE;
  }
  if (img->color_type != COLOR_TYPE_RGB) {
    // 画像形式がRGBでない場合はRGBに変換して出力
    to_free = clone_image(img);
    img = image_to_rgb(to_free);
  }
  jpegc.err = jpeg_std_error(&myerr.jerr);
  myerr.jerr.error_exit = error_exit;
  if (setjmp(myerr.jmpbuf)) {
    goto error;
  }
  jpeg_create_compress(&jpegc);
  jpeg_stdio_dest(&jpegc, fp);
  jpegc.image_width = img->width;
  jpegc.image_height = img->height;
  jpegc.input_components = 3;
  jpegc.in_color_space = JCS_RGB;
  jpeg_set_defaults(&jpegc);
  jpeg_set_quality(&jpegc, quality, TRUE);
  jpeg_start_compress(&jpegc, TRUE);
  for (y = 0; y < img->height; y++) {
    row = buffer;
    for (x = 0; x < img->width; x++) {
      *row++ = img->map[y][x].c.r;
      *row++ = img->map[y][x].c.g;
      *row++ = img->map[y][x].c.b;
    }
    jpeg_write_scanlines(&jpegc, &buffer, 1);
  }
  jpeg_finish_compress(&jpegc);
  result = SUCCESS;
  error:
  jpeg_destroy_compress(&jpegc);
  free(buffer);
  free_image(to_free);
  return result;
}

//by totsucom
//書き出し時に回転できる
result_t write_jpeg_streamEx(FILE *fp, image_t *img, int quality, int rotation) {
  switch (rotation) {
    case ROTATE_CLOCKWISE_0:   break;
    case ROTATE_CLOCKWISE_90:  break;
    case ROTATE_CLOCKWISE_180: break;
    case ROTATE_CLOCKWISE_270: break;
    default: return FAILURE;
  }
  result_t result = FAILURE;
  int x, y;
  struct jpeg_compress_struct jpegc;
  my_error_mgr myerr;
  image_t *to_free = NULL;
  JSAMPROW buffer = NULL;
  JSAMPROW row;
  if (img == NULL) {
    return FAILURE;
  }
  if ((buffer = malloc(sizeof(JSAMPLE) * 3 * img->width)) == NULL) {
    return FAILURE;
  }
  if (img->color_type != COLOR_TYPE_RGB) {
    // 画像形式がRGBでない場合はRGBに変換して出力
    to_free = clone_image(img);
    img = image_to_rgb(to_free);
  }
  jpegc.err = jpeg_std_error(&myerr.jerr);
  myerr.jerr.error_exit = error_exit;
  if (setjmp(myerr.jmpbuf)) {
    goto error;
  }
  jpeg_create_compress(&jpegc);
  jpeg_stdio_dest(&jpegc, fp);
  if (rotation == ROTATE_CLOCKWISE_0 || rotation == ROTATE_CLOCKWISE_180) {
    jpegc.image_width = img->width;
    jpegc.image_height = img->height;
  } else {
    jpegc.image_width = img->height;
    jpegc.image_height = img->width;
  }
  jpegc.input_components = 3;
  jpegc.in_color_space = JCS_RGB;
  jpeg_set_defaults(&jpegc);
  jpeg_set_quality(&jpegc, quality, TRUE);
  jpeg_start_compress(&jpegc, TRUE);
  switch (rotation) {
    case ROTATE_CLOCKWISE_0:
      for (y = 0; y < img->height; y++) {
        row = buffer;
        for (x = 0; x < img->width; x++) {
          *row++ = img->map[y][x].c.r;
          *row++ = img->map[y][x].c.g;
          *row++ = img->map[y][x].c.b;
        }
        jpeg_write_scanlines(&jpegc, &buffer, 1);
      }
      break;
    case ROTATE_CLOCKWISE_90:
      for (x = 0; x < img->width; x++) {
        row = buffer;
        for (y = img->height - 1; y >= 0; y--) {
          *row++ = img->map[y][x].c.r;
          *row++ = img->map[y][x].c.g;
          *row++ = img->map[y][x].c.b;
        }
        jpeg_write_scanlines(&jpegc, &buffer, 1);
      }
      break;
    case ROTATE_CLOCKWISE_180:
      for (y = img->height - 1; y >= 0; y--) {
        row = buffer;
        for (x = img->width - 1; x >= 0; x--) {
          *row++ = img->map[y][x].c.r;
          *row++ = img->map[y][x].c.g;
          *row++ = img->map[y][x].c.b;
        }
        jpeg_write_scanlines(&jpegc, &buffer, 1);
      }
      break;
    default: //ROTATE_CLOCKWISE_270
      for (x = img->width - 1; x >= 0; x--) {
        row = buffer;
        for (y = 0; y < img->height; y++) {
          *row++ = img->map[y][x].c.r;
          *row++ = img->map[y][x].c.g;
          *row++ = img->map[y][x].c.b;
        }
        jpeg_write_scanlines(&jpegc, &buffer, 1);
      }
      break;
  }
  jpeg_finish_compress(&jpegc);
  result = SUCCESS;
  error:
  jpeg_destroy_compress(&jpegc);
  free(buffer);
  free_image(to_free);
  return result;
}
