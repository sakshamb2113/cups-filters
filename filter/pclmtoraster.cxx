/**
 * This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * @brief Decode PCLm to a Raster file
 * @file pclmtoraster.cxx
 * @author Vikrant Malik <vikrantmalik051@gmail.com> (c) 2020
 */

#include <stdio.h>
#include <stdlib.h>
#include <cups/raster.h>
#include <cups/cups.h>
#include <ppd/ppd.h>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <cupsfilters/raster.h>
#include <cupsfilters/image.h>
#include <cupsfilters/bitmap.h>

#if (CUPS_VERSION_MAJOR > 1) || (CUPS_VERSION_MINOR > 6)
#define HAVE_CUPS_1_7 1
#endif
#define MAX_BYTES_PER_PIXEL 32

namespace {
  typedef unsigned char *(*ConvertCSpace)(unsigned char *src, unsigned char *dst, unsigned int row,
					unsigned int pixels);
  typedef unsigned char *(*ConvertLine)(unsigned char *src, unsigned char *dst, unsigned char *buf,
					unsigned int row, unsigned int plane, unsigned int pixels);
  ConvertCSpace convertcspace;
  ConvertLine convertline;
  int pwgraster = 0;
  int numcolors = 0;
  int rowsize = 0;
  cups_page_header2_t header;
  ppd_file_t *ppd = 0;
  char pageSizeRequested[64];
  int bi_level = 0;
  /* image swapping */
  bool swap_image_x = false;
  bool swap_image_y = false;
  /* margin swapping */
  bool swap_margin_x = false;
  bool swap_margin_y = false;
  unsigned int nplanes;
  unsigned int nbands;
  unsigned int bytesPerLine; /* number of bytes per line */
                        /* Note: When CUPS_ORDER_BANDED,
                           cupsBytesPerLine = bytesPerLine*cupsNumColors */
}

static void parseOpts(int argc, char **argv)
{
  int           num_options = 0;
  cups_option_t*options = NULL;
  const char*   t = NULL;
  ppd_attr_t*   attr;
  const char	*val;

#ifdef HAVE_CUPS_1_7
  t = getenv("FINAL_CONTENT_TYPE");
  if (t && strcasestr(t, "pwg"))
    pwgraster = 1;
#endif /* HAVE_CUPS_1_7 */

  ppd = ppdOpenFile(getenv("PPD"));
  if (ppd == NULL)
    fprintf(stderr, "DEBUG: PPD file is not specified.\n");
  if (ppd)
    ppdMarkDefaults(ppd);
  //Parse IPP options from command lines
  num_options = cupsParseOptions(argv[5],0,&options);
  if (ppd) {
    ppdMarkDefaults(ppd);
    ppdMarkOptions(ppd,num_options,options);
//     handleRqeuiresPageRegion();
    ppdRasterInterpretPPD(&header,ppd,num_options,options,0);
    if (header.Duplex) {
      /* analyze options relevant to Duplex */
      const char *backside = "";
      /* APDuplexRequiresFlippedMargin */
      enum {
        FM_NO, FM_FALSE, FM_TRUE
      } flippedMargin = FM_NO;

      attr = ppdFindAttr(ppd,"cupsBackSide",NULL);
      if (attr != NULL && attr->value != NULL) {
        ppd->flip_duplex = 0;
        backside = attr->value;
      } else if (ppd->flip_duplex) {
        backside = "Rotated"; /* compatible with Max OS and GS 8.71 */
      }

      attr = ppdFindAttr(ppd,"APDuplexRequiresFlippedMargin",NULL);
      if (attr != NULL && attr->value != NULL) {
        if (strcasecmp(attr->value,"true") == 0) {
          flippedMargin = FM_TRUE;
        } else {
          flippedMargin = FM_FALSE;
        }
      }
      if (strcasecmp(backside,"ManualTumble") == 0 && header.Tumble) {
        swap_image_x = swap_image_y = true;
        swap_margin_x = swap_margin_y = true;
        if (flippedMargin == FM_TRUE) {
          swap_margin_y = false;
        }
      } else if (strcasecmp(backside,"Rotated") == 0 && !header.Tumble) {
        swap_image_x = swap_image_y = true;
        swap_margin_x = swap_margin_y = true;
        if (flippedMargin == FM_TRUE) {
          swap_margin_y = false;
        }
      } else if (strcasecmp(backside,"Flipped") == 0) {
        if (header.Tumble) {
          swap_image_x = true;
          swap_margin_x = swap_margin_y = true;
        } else {
          swap_image_y = true;
        }
        if (flippedMargin == FM_FALSE) {
          swap_margin_y = !swap_margin_y;
        }
      }
    }

#ifdef HAVE_CUPS_1_7
    if ((attr = ppdFindAttr(ppd,"PWGRaster",0)) != 0 &&
        (!strcasecmp(attr->value, "true")
         || !strcasecmp(attr->value, "on") ||
         !strcasecmp(attr->value, "yes")))
      pwgraster = 1;
    if (pwgraster == 1)
      cupsRasterParseIPPOptions(&header, num_options, options, pwgraster, 0);
#endif /* HAVE_CUPS_1_7 */
  } else {
#ifdef HAVE_CUPS_1_7
    pwgraster = 1;
    t = cupsGetOption("media-class", num_options, options);
    if (t == NULL)
      t = cupsGetOption("MediaClass", num_options, options);
    if (t != NULL)
    {
      if (strcasestr(t, "pwg"))
        pwgraster = 1;
      else
        pwgraster = 0;
    }
    cupsRasterParseIPPOptions(&header,num_options,options,pwgraster,1);
#else
    fprintf(stderr, "ERROR: No PPD file specified.\n");
    exit(1);
#endif /* HAVE_CUPS_1_7 */
  }
  if ((val = cupsGetOption("print-color-mode", num_options, options)) != NULL
                           && !strncasecmp(val, "bi-level", 8))
    bi_level = 1;

  strncpy(pageSizeRequested, header.cupsPageSizeName, 64);
  fprintf(stderr, "DEBUG: Page size requested: %s\n", header.cupsPageSizeName);
}

static bool mediaboxlookup(QPDFObjectHandle object, float rect[4])
{
  // preliminary checks
  if (!object.isDictionary() || !object.hasKey("/MediaBox"))
    return false;

  // assign mediabox values to rect
  std::vector<QPDFObjectHandle> mediabox = object.getKey("/MediaBox").getArrayAsVector();
  for (int i = 0; i < 4; ++i) {
    rect[i] = mediabox[i].getNumericValue();
  }

  return mediabox.size() == 4;
}

static unsigned char *rotatebitmap(unsigned char *src, unsigned char *dst,
     unsigned int rotate, unsigned int height, unsigned int width, int rowsize, std::string colorspace)
{
  unsigned char *bp = src;
  unsigned char *dp = dst;
  unsigned char *temp = dst;

  if (rotate == 0) {
    return src;
  } else if (rotate == 180) {
    if (colorspace == "/DeviceGray") {
      bp = src + height * rowsize - 1;
      dp = dst;
      for (unsigned int h = 0; h < height; h++) {
        for (unsigned int w = 0; w < width; w++, bp --, dp ++) {
          *dp = *bp;
        }
      }
    } else if (colorspace == "/DeviceCMYK") {
      bp = src + height * rowsize - 4;
      dp = dst;
      for (unsigned int h = 0; h < height; h++) {
        for (unsigned int w = 0; w < width; w++, bp -= 4, dp += 4) {
          dp[0] = bp[0];
          dp[1] = bp[1];
          dp[2] = bp[2];
          dp[3] = bp[3];
        }
      }
    } else if (colorspace == "/DeviceRGB") {
      bp = src + height * rowsize - 3;
      dp = dst;
      for (unsigned int h = 0; h < height; h++) {
        for (unsigned int w = 0; w < width; w++, bp -= 3, dp += 3) {
          dp[0] = bp[0];
          dp[1] = bp[1];
          dp[2] = bp[2];
        }
      }
    }
  }
  else if (rotate == 270) {
    if (colorspace == "/DeviceGray") {
      bp = src;
      dp = dst;
      for (unsigned int h = 0; h < height; h++) {
        bp = src + (height - h) - 1;
        for (unsigned int w = 0; w < width; w++, bp += height , dp ++) {
          *dp = *bp;
        }
      }
    } else if (colorspace == "/DeviceCMYK") {
      for (unsigned int h = 0; h < height; h++) {
        bp = src + (height - h)*4 - 4;
        for (unsigned int i = 0; i < width; i++, bp += height*4 , dp += 4) {
          dp[0] = bp[0];
          dp[1] = bp[1];
          dp[2] = bp[2];
          dp[3] = bp[3];
        }
      }
    } else if (colorspace == "/DeviceRGB") {
      bp = src;
      dp = dst;
      for (unsigned int h = 0; h < height; h++) {
        bp = src + (height - h)*3 - 3;
        for (unsigned int i = 0; i < width; i++, bp += height*3 , dp += 3) {
          dp[0] = bp[0];
          dp[1] = bp[1];
          dp[2] = bp[2];
        }
      }
    }
  }
  else if (rotate == 90) {
    if (colorspace == "/DeviceGray") {
      for (unsigned int h = 0; h < height; h++) {
        bp = src + (width - 1) * height + h;
        for (unsigned int i = 0; i < width; i++, bp -= height , dp ++) {
          *dp = *bp;
        }
      }
    } else if (colorspace == "/DeviceCMYK") {
      for (unsigned int h = 0; h < height; h++) {
        bp = src + (width - 1) * height * 4 + 4*h;
        for (unsigned int i = 0; i < width; i++, bp -= height*4 , dp += 4) {
          dp[0] = bp[0];
          dp[1] = bp[1];
          dp[2] = bp[2];
          dp[3] = bp[3];
        }
      }
    } else if (colorspace == "/DeviceRGB") {
      for (unsigned int h = 0; h < height; h++) {
       bp = src + (width - 1) * height * 3 + 3*h;
        for (unsigned int i = 0; i < width; i++, bp -= height*3 , dp += 3) {
          dp[0] = bp[0];
          dp[1] = bp[1];
          dp[2] = bp[2];
        }
      }
    }
  }
  else {
    fprintf(stderr, "ERROR: Incorrect Rotate Value %d\n", rotate);
    exit(1);
  }

  return temp;
}

static unsigned char *RGBtoCMYKLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  cupsImageRGBToCMYK(src,dst,pixels);
  return dst;
}

static unsigned char *RGBtoCMYLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  cupsImageRGBToCMY(src,dst,pixels);
  return dst;
}

static unsigned char *RGBtoWhiteLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  if (header.cupsBitsPerColor != 1) {
    cupsImageRGBToWhite(src,dst,pixels);
  } else {
    cupsImageRGBToWhite(src,src,pixels);
    oneBitLine(src, dst, header.cupsWidth, row, bi_level);
  }

  return dst;
}

static unsigned char *RGBtoBlackLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  if (header.cupsBitsPerColor != 1) {
    cupsImageRGBToBlack(src,dst,pixels);
  } else {
    cupsImageRGBToBlack(src,src,pixels);
    oneBitLine(src, dst, header.cupsWidth, row, bi_level);
  }
  return dst;
}

static unsigned char *CMYKtoRGBLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  cupsImageCMYKToRGB(src,dst,pixels);
  return dst;
}

static unsigned char *CMYKtoCMYLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  // Converted first to rgb and then to cmy for better outputs.
  cupsImageCMYKToRGB(src,src,pixels);
  cupsImageRGBToCMY(src,dst,pixels);
  return dst;
}

static unsigned char *CMYKtoWhiteLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  if (header.cupsBitsPerColor != 1) {
    cupsImageCMYKToWhite(src,dst,pixels);
  } else {
    cupsImageCMYKToWhite(src,src,pixels);
    oneBitLine(src, dst, header.cupsWidth, row, bi_level);
  }
  return dst;
}

static unsigned char *CMYKtoBlackLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  if (header.cupsBitsPerColor != 1) {
    cupsImageCMYKToBlack(src,dst,pixels);
  } else {
    cupsImageCMYKToBlack(src,src,pixels);
    oneBitLine(src, dst, header.cupsWidth, row, bi_level);
  }
  return dst;
}

static unsigned char *GraytoRGBLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  cupsImageWhiteToRGB(src,dst,pixels);
  return dst;
}

static unsigned char *GraytoCMYKLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  cupsImageWhiteToCMYK(src,dst,pixels);
  return dst;
}

static unsigned char *GraytoCMYLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  cupsImageWhiteToCMY(src,dst,pixels);
  return dst;
}

static unsigned char *GraytoBlackLine(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  if (header.cupsBitsPerColor != 1) {
    cupsImageWhiteToBlack(src, dst, pixels);
  } else {
    cupsImageWhiteToBlack(src, src, pixels);
    oneBitLine(src, dst, header.cupsWidth, row, bi_level);
  }
  return dst;
}

static unsigned char *convertcspaceNoop(unsigned char *src, unsigned char *dst, unsigned int row, unsigned int pixels)
{
  return src;
}

static unsigned char *convertLine(unsigned char *src, unsigned char *dst,
     unsigned char *buf, unsigned int row, unsigned int plane, unsigned int pixels)
{
  /*
   Use only convertcspace if conversion of bits and conversion of color order
   is not required, or if dithering is required, for faster processing of raster output.
   */
  if ((header.cupsBitsPerColor == 1
	&& header.cupsNumColors == 1)
	|| (header.cupsBitsPerColor == 8
	&& header.cupsColorOrder == CUPS_ORDER_CHUNKED)) {
    dst = convertcspace(src, dst, row, pixels);
  } else {
    for (unsigned int i = 0;i < pixels;i++) {
      unsigned char pixelBuf1[MAX_BYTES_PER_PIXEL];
      unsigned char pixelBuf2[MAX_BYTES_PER_PIXEL];
      unsigned char *pb;
      pb = convertcspace(src + i*numcolors, pixelBuf1, row, 1);
      pb = convertbits(pb, pixelBuf2, i, row, header.cupsNumColors, header.cupsBitsPerColor);
      writepixel(dst, plane, i, pb, header.cupsNumColors, header.cupsBitsPerColor, header.cupsColorOrder);
    }
  }
  return dst;
}

static unsigned char *convertReverseLine(unsigned char *src, unsigned char *dst,
     unsigned char *buf, unsigned int row, unsigned int plane, unsigned int pixels)
{
  // Use only convertcspace if conversion of bits and conversion of color order
  // is not required, or if dithering is required, for faster processing of raster output.
  if (header.cupsBitsPerColor == 1 && header.cupsNumColors == 1) {
    buf = convertcspace(src, buf, row, pixels);
    dst = reverseOneBitLine(buf, dst, pixels, bytesPerLine);
  } else if (header.cupsBitsPerColor == 8 && header.cupsColorOrder == CUPS_ORDER_CHUNKED) {
    unsigned char *dp = dst;
    // Assign each pixel of buf to dst in the reverse order.
    buf = convertcspace(src, buf, row, pixels) + (header.cupsWidth - 1)*header.cupsNumColors;
    for (unsigned int i = 0; i < pixels; i++, buf-=header.cupsNumColors, dp+=header.cupsNumColors) {
      for (unsigned int j = 0; j < header.cupsNumColors; j++) {
	dp[j] = buf[j];
      }
    }
  } else {
    for (unsigned int i = 0;i < pixels;i++) {
      unsigned char pixelBuf1[MAX_BYTES_PER_PIXEL];
      unsigned char pixelBuf2[MAX_BYTES_PER_PIXEL];
      unsigned char *pb;
      pb = convertcspace(src + (pixels - i - 1)*numcolors, pixelBuf1, row, 1);
      pb = convertbits(pb, pixelBuf2, i, row, header.cupsNumColors, header.cupsBitsPerColor);
      writepixel(dst, plane, i, pb, header.cupsNumColors, header.cupsBitsPerColor, header.cupsColorOrder);
    }
  }
  return dst;
}

static void selectConvertFunc (std::string colorspace, int pgno) {

  /* Set rowsize and numcolors based on colorspace of raster data */
  if (colorspace == "/DeviceRGB") {
    rowsize = header.cupsWidth*3;
    numcolors = 3;
  } else if (colorspace == "/DeviceCMYK") {
    rowsize = header.cupsWidth*4;
    numcolors = 4;
  } else if (colorspace == "/DeviceGray") {
    rowsize = header.cupsWidth;
    numcolors = 1;
  } else {
    fprintf(stderr, "ERROR: Colorspace %s not supported\n", colorspace.c_str());
    exit(1);
  }

  convertcspace = convertcspaceNoop; //Default function
  /* Select convertcspace function */
  switch (header.cupsColorSpace) {
    case CUPS_CSPACE_K:
     if (colorspace == "/DeviceRGB") convertcspace = RGBtoBlackLine;
     else if (colorspace == "/DeviceCMYK") convertcspace = CMYKtoBlackLine;
     else if (colorspace == "/DeviceGray") convertcspace = GraytoBlackLine;
     break;
    case CUPS_CSPACE_W:
    case CUPS_CSPACE_SW:
     if (colorspace == "/DeviceRGB") convertcspace = RGBtoWhiteLine;
     else if (colorspace == "/DeviceCMYK") convertcspace = CMYKtoWhiteLine;
     break;
    case CUPS_CSPACE_CMY:
     if (colorspace == "/DeviceRGB") convertcspace = RGBtoCMYLine;
     else if (colorspace == "/DeviceCMYK") convertcspace = CMYKtoCMYLine;
     else if (colorspace == "/DeviceGray") convertcspace = GraytoCMYLine;
     break;
    case CUPS_CSPACE_CMYK:
     if (colorspace == "/DeviceRGB") convertcspace = RGBtoCMYKLine;
     else if (colorspace == "/DeviceGray") convertcspace = GraytoCMYKLine;
     break;
    case CUPS_CSPACE_RGB:
    case CUPS_CSPACE_ADOBERGB:
    case CUPS_CSPACE_SRGB:
    default:
     if (colorspace == "/DeviceCMYK") convertcspace = CMYKtoRGBLine;
     else if (colorspace == "/DeviceGray") convertcspace = GraytoRGBLine;
     break;
   }

  /* Select convertline function */
  if (header.Duplex && (pgno & 1) && swap_image_x) {
    convertline = convertReverseLine;
  } else {
    convertline = convertLine;
  }

}

static void outPage(cups_raster_t *raster, QPDFObjectHandle page, int pgno) {
  long long		rotate = 0,
			height,
			width;
  double		paperdimensions[2], margins[4], l, swap;
  int			bufsize = 0, pixel_count = 0,
			temp = 0;
  float 		mediaBox[4];
  unsigned char 	*bitmap = NULL,
			*colordata = NULL,
			*lineBuf = NULL,
			*line = NULL,
			*dp = NULL;
  std::string		colorspace;
  QPDFObjectHandle	image;
  QPDFObjectHandle	imgdict;
  QPDFObjectHandle	colorspace_obj;

  // Check if page is rotated.
  if (page.getKey("/Rotate").isInteger())
    rotate = page.getKey("/Rotate").getIntValueAsInt();

  // Get pagesize by the mediabox key of the page.
  if (!mediaboxlookup(page, mediaBox)){
    fprintf(stderr, "ERROR: pdf page %d doesn't contain a valid mediaBox\n", pgno + 1);
    return;
  } else {
    fprintf(stderr, "DEBUG: mediaBox = [%f %f %f %f];\n", mediaBox[0], mediaBox[1], mediaBox[2], mediaBox[3]);
    l = mediaBox[2] - mediaBox[0];
    if (l < 0) l = -l;
    if (rotate == 90 || rotate == 270)
      header.PageSize[1] = (unsigned)l;
    else
      header.PageSize[0] = (unsigned)l;
    l = mediaBox[3] - mediaBox[1];
    if (l < 0) l = -l;
    if (rotate == 90 || rotate == 270)
      header.PageSize[0] = (unsigned)l;
    else
      header.PageSize[1] = (unsigned)l;
  }

  // Adjust header page size and margins according to the ppd file.
  if (ppd) {
    ppdRasterMatchPPDSize(&header, ppd, margins, paperdimensions, NULL, NULL);
    if (pwgraster == 1)
      memset(margins, 0, sizeof(margins));
  } else {
    for (int i = 0; i < 2; i ++)
      paperdimensions[i] = header.PageSize[i];
    if (header.cupsImagingBBox[3] > 0.0) {
      /* Set margins if we have a bounding box defined ... */
      if (pwgraster == 0) {
	margins[0] = header.cupsImagingBBox[0];
	margins[1] = header.cupsImagingBBox[1];
	margins[2] = paperdimensions[0] - header.cupsImagingBBox[2];
	margins[3] = paperdimensions[1] - header.cupsImagingBBox[3];
      }
    } else
      /* ... otherwise use zero margins */
      for (int i = 0; i < 4; i ++)
	margins[i] = 0.0;
  }

  if (header.Duplex && (pgno & 1)) {
    /* backside: change margin if needed */
    if (swap_margin_x) {
      swap = margins[2]; margins[2] = margins[0]; margins[0] = swap;
    }
    if (swap_margin_y) {
      swap = margins[3]; margins[3] = margins[1]; margins[1] = swap;
    }
  }

  /* write page header */
  for (int i = 0; i < 2; i ++) {
    header.cupsPageSize[i] = paperdimensions[i];
    header.PageSize[i] = (unsigned int)(header.cupsPageSize[i] + 0.5);
    if (pwgraster == 0)
      header.Margins[i] = margins[i] + 0.5;
    else
      header.Margins[i] = 0;
  }
  if (pwgraster == 0) {
    header.cupsImagingBBox[0] = margins[0];
    header.cupsImagingBBox[1] = margins[1];
    header.cupsImagingBBox[2] = paperdimensions[0] - margins[2];
    header.cupsImagingBBox[3] = paperdimensions[1] - margins[3];
    for (int i = 0; i < 4; i ++)
      header.ImagingBoundingBox[i] = (unsigned int)(header.cupsImagingBBox[i] + 0.5);
  } else
    for (int i = 0; i < 4; i ++) {
      header.cupsImagingBBox[i] = 0.0;
      header.ImagingBoundingBox[i] = 0;
    }

  header.cupsWidth = 0;
  header.cupsHeight = 0;

  /* Loop over all raster images in a page and store them in bitmap. */
  std::map<std::string, QPDFObjectHandle> images = page.getPageImages();
  for (auto const& iter: images) {
    image = iter.second;
    imgdict = image.getDict(); //XObject dictionary

    PointerHolder<Buffer> actual_data = image.getStreamData(qpdf_dl_all);
    width = imgdict.getKey("/Width").getIntValue();
    height = imgdict.getKey("/Height").getIntValue();
    colorspace_obj = imgdict.getKey("/ColorSpace");
    header.cupsHeight += height;
    bufsize = actual_data->getSize();

    if(!pixel_count) {
      bitmap = (unsigned char *) malloc(bufsize);
    }
    else {
      bitmap = (unsigned char *) realloc(bitmap, pixel_count + bufsize);
    }
    memcpy(bitmap + pixel_count, actual_data->getBuffer(), bufsize);
    pixel_count += bufsize;

    if (width > header.cupsWidth) header.cupsWidth = width;
  }

  // Swap width and height in landscape images
  if (rotate == 270 || rotate == 90) {
    temp = header.cupsHeight;
    header.cupsHeight = header.cupsWidth;
    header.cupsWidth = temp;
  }

  bytesPerLine = header.cupsBytesPerLine = (header.cupsBitsPerPixel * header.cupsWidth + 7) / 8;
  if (header.cupsColorOrder == CUPS_ORDER_BANDED) {
    header.cupsBytesPerLine *= header.cupsNumColors;
  }

  if (!cupsRasterWriteHeader2(raster,&header)) {
    fprintf(stderr, "ERROR: Can't write page %d header\n", pgno + 1);
    exit(1);
  }

  colorspace = (colorspace_obj.isName() ? colorspace_obj.getName() : "/DeviceRGB"); // Default for pclm files in DeviceRGB

  // If page is to be swapped in both x and y, rotate it by 180 degress
  if (header.Duplex && (pgno & 1) && swap_image_y && swap_image_x) {
    rotate = (rotate + 180) % 360;
    swap_image_y = false;
    swap_image_x = false;
  }

  /* Rotate Bitmap */
  if (rotate) {
    unsigned char *bitmap2 = (unsigned char *) malloc(pixel_count);
    bitmap2 = rotatebitmap(bitmap, bitmap2, rotate, header.cupsHeight, header.cupsWidth, rowsize, colorspace);
    free(bitmap);
    bitmap = bitmap2;
  }

  colordata = bitmap;

  /* Select convertline and convertscpace function */
  selectConvertFunc(colorspace, pgno);

  /* Write page image */
  lineBuf = new unsigned char [bytesPerLine];
  line = new unsigned char [bytesPerLine];
  if (header.Duplex && (pgno & 1) && swap_image_y) {
    for (unsigned int plane = 0; plane < nplanes ; plane++) {
      unsigned char *bp = colordata + (header.cupsHeight - 1) * rowsize;
      for (unsigned int h = header.cupsHeight; h > 0; h--) {
        for (unsigned int band = 0; band < nbands; band++) {
         dp = convertline(bp, line, lineBuf, h - 1, plane + band, header.cupsWidth);
         cupsRasterWritePixels(raster, dp, bytesPerLine);
        }
       bp -= rowsize;
      }
    }
  } else {
    for (unsigned int plane = 0; plane < nplanes ; plane++) {
      unsigned char *bp = colordata;
      for (unsigned int h = 0; h < header.cupsHeight; h++) {
        for (unsigned int band = 0; band < nbands; band++) {
         dp = convertline(bp, line, lineBuf, h, plane + band, header.cupsWidth);
         cupsRasterWritePixels(raster, dp, bytesPerLine);
        }
       bp += rowsize;
      }
    }
  }
  delete[] lineBuf;
  delete[] line;
  free(bitmap);
}

int main(int argc, char **argv)
{
  int   	npages=0;
  QPDF  	*pdf = new QPDF();
  cups_raster_t *raster;


  if (argc == 6) {
    /* stdin input, copy to temporary file */
    int fd;
    char name[BUFSIZ];
    char buf[BUFSIZ];
    int n;

    fd = cupsTempFd(name,sizeof(name));
    if (fd < 0) {
      fprintf(stderr, "ERROR: Can't create temporary file\n");
      exit(1);
    }

    /* copy stdin to the tmp file */
    while ((n = read(0,buf,BUFSIZ)) > 0) {
      if (write(fd,buf,n) != n) {
        fprintf(stderr, "ERROR: Can't copy stdin to temporary file\n" );
        close(fd);
	exit(1);
      }
    }
    close(fd);
    pdf->processFile(name);
    /* remove name */
    unlink(name);
  } else if (argc == 7) {
    FILE *fp;
    if ((fp = fopen(argv[6],"rb")) == 0) {
        fprintf(stderr, "ERROR: Can't open input file %s\n",argv[6]);
        exit(1);
    }
    pdf->processFile(argv[6]);
    fclose(fp);
  } else {
    fprintf(stderr, "ERROR: Usage: %s job-id user title copies options [file]\n",
            argv[0]);
    exit(1);
  }

  parseOpts(argc, argv);

  if (header.cupsBitsPerColor != 1
     && header.cupsBitsPerColor != 2
     && header.cupsBitsPerColor != 4
     && header.cupsBitsPerColor != 8
     && header.cupsBitsPerColor != 16) {
    fprintf(stderr, "ERROR: Specified color format is not supported\n");
    exit(1);
  }

  if (header.cupsColorOrder == CUPS_ORDER_PLANAR) {
    nplanes = header.cupsNumColors;
  } else {
    nplanes = 1;
  }
  if (header.cupsColorOrder == CUPS_ORDER_BANDED) {
    nbands = header.cupsNumColors;
  } else {
    nbands = 1;
  }

  if ((raster = cupsRasterOpen(1, pwgraster ? CUPS_RASTER_WRITE_PWG :
                               CUPS_RASTER_WRITE)) == 0) {
        fprintf(stderr, "ERROR: Can't open raster stream\n");
        exit(1);
  }

  std::vector<QPDFObjectHandle> pages = pdf->getAllPages();
  npages = pages.size();

  for (int i = 0;
       i < npages; ++i) {
       fprintf(stderr, "INFO: Starting page %d.\n", i + 1);
       outPage(raster, pages[i], i);
    }

  cupsRasterClose(raster);
  delete pdf;
  if (ppd != NULL)  ppdClose(ppd);
  return 0;
}

void operator delete[](void *p) throw ()
{
  free(p);
}
