/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2007 The Zdeno Ash Miklas. */

/** \file gameengine/VideoTexture/ImageViewport.cpp
 *  \ingroup bgevideotex
 */

// implementation

#include "ImageViewport.h"

#include "GPU_framebuffer.hh"
#include "GPU_state.hh"

#include "FilterSource.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "RAS_ICanvas.h"
#include "Texture.h"

ImageViewport::ImageViewport() : m_alpha(false), m_texInit(false)
{
  /* Because this constructor is called from python direclty without any arguments
   * the viewport should be the one of the final screen with gaps.
   * To do this we use the canvas viewport.
   */

  RAS_ICanvas *canvas = KX_GetActiveEngine()->GetCanvas();
  const RAS_Rect &viewport = canvas->GetViewportArea();

  m_viewport[0] = viewport.GetLeft();
  m_viewport[1] = viewport.GetBottom();
  m_viewport[2] = viewport.GetRight();
  m_viewport[3] = viewport.GetTop();

  m_width = m_viewport[2] - m_viewport[0];
  m_height = m_viewport[3] - m_viewport[1];

  // glGetIntegerv(GL_VIEWPORT, m_viewport);
  // create buffer for viewport image
  // Warning: this buffer is also used to get the depth buffer as an array of
  //          float (1 float = 4 bytes per pixel)
  m_viewportImage = new BYTE[4 * getViewportSize()[0] * getViewportSize()[1]];
  // set attributes
  setWhole(true);
}

// constructor
ImageViewport::ImageViewport(unsigned int width, unsigned int height)
    : m_width(width), m_height(height), m_alpha(false), m_texInit(false)
{
  m_viewport[0] = 0;
  m_viewport[1] = 0;
  m_viewport[2] = m_width;
  m_viewport[3] = m_height;

  // glGetIntegerv(GL_VIEWPORT, m_viewport);
  // create buffer for viewport image
  // Warning: this buffer is also used to get the depth buffer as an array of
  //          float (1 float = 4 bytes per pixel)
  m_viewportImage = new BYTE[4 * getViewportSize()[0] * getViewportSize()[1]];
  // set attributes
  setWhole(true);
}

// destructor
ImageViewport::~ImageViewport(void)
{
  delete[] m_viewportImage;
}

// use whole viewport to capture image
void ImageViewport::setWhole(bool whole)
{
  // set whole
  m_whole = whole;
  // set capture size to viewport size, if whole,
  // otherwise place area in the middle of viewport
  for (int idx = 0; idx < 2; ++idx) {
    // capture size
    m_capSize[idx] = whole ? short(getViewportSize()[idx]) :
                             calcSize(short(getViewportSize()[idx]));
    // position
    m_position[idx] = whole ? 0 : ((getViewportSize()[idx] - m_capSize[idx]) >> 1);
  }
  // init image
  init(m_capSize[0], m_capSize[1]);
  // set capture position
  setPosition();
}

void ImageViewport::setCaptureSize(short size[2])
{
  m_whole = false;
  if (size == nullptr)
    size = m_capSize;
  for (int idx = 0; idx < 2; ++idx) {
    if (size[idx] < 1)
      m_capSize[idx] = 1;
    else if (size[idx] > getViewportSize()[idx])
      m_capSize[idx] = short(getViewportSize()[idx]);
    else
      m_capSize[idx] = size[idx];
  }
  init(m_capSize[0], m_capSize[1]);
  // set capture position
  setPosition();
}

// set position of capture rectangle
void ImageViewport::setPosition(int pos[2])
{
  // if new position is not provided, use existing position
  if (pos == nullptr)
    pos = m_position;
  // save position
  for (int idx = 0; idx < 2; ++idx)
    m_position[idx] = pos[idx] < 0 ? 0 :
                      pos[idx] >= getViewportSize()[idx] - m_capSize[idx] ?
                                     getViewportSize()[idx] - m_capSize[idx] :
                                     pos[idx];
  // recalc up left corner
  for (int idx = 0; idx < 2; ++idx)
    m_upLeft[idx] = m_position[idx] + m_viewport[idx];
}

// capture image from viewport
void ImageViewport::calcViewport(unsigned int textid, double ts)
{
  GPUFrameBuffer *target = GPU_framebuffer_active_get();
  // if scale was changed
  if (m_scaleChange)
    // reset image
    init(m_capSize[0], m_capSize[1]);
  // if texture wasn't initialized
  if (!m_texInit) {
    // initialize it
    if (m_texture) {
      m_texture->loadTexture(m_image, m_size, false, m_internalFormat);
      m_texInit = true;
    }
  }
  // otherwise copy viewport to buffer, if image is not available
  if (!m_avail) {
    if (m_zbuff) {
      // Use read pixels with the depth buffer
      // *** misusing m_viewportImage here, but since it has the correct size
      //     (4 bytes per pixel = size of float) and we just need it to apply
      //     the filter, it's ok
      GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
      float *depth_buffer = (float *)GPU_texture_read(
          GPU_framebuffer_depth_texture(target), GPU_DATA_FLOAT, 0);
      if (depth_buffer) {
        std::memcpy(m_viewportImage, depth_buffer, sizeof(float) * m_capSize[0] * m_capSize[1]);
        FilterZZZA filt;
        filterImage(filt, (float *)m_viewportImage, m_capSize);
        MEM_delete(depth_buffer);
      }
      else {
        if (m_depth) {
          // Use read pixels with the depth buffer
          // See warning above about m_viewportImage.
          GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
          float *depth_buffer = (float *)GPU_texture_read(
              GPU_framebuffer_depth_texture(target), GPU_DATA_FLOAT, 0);
          if (depth_buffer) {
            std::memcpy(
                m_viewportImage, depth_buffer, sizeof(float) * m_capSize[0] * m_capSize[1]);
            // filter loaded data
            FilterDEPTH filt;
            filterImage(filt, (float *)m_viewportImage, m_capSize);
            MEM_delete(depth_buffer);
          }
        }
        else {
          // get frame buffer data
          if (m_alpha) {
            // as we are reading the pixel in the native format, we can read directly in the image
            // buffer if we are sure that no processing is needed on the image
            GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
            float *color_buffer = (float *)GPU_texture_read(
                GPU_framebuffer_color_texture(target), GPU_DATA_FLOAT, 0);
            if (color_buffer) {
              if (m_size[0] == m_capSize[0] && m_size[1] == m_capSize[1] && !m_flip && !m_pyfilter)
              {
                std::memcpy(m_image, color_buffer, 4 * m_capSize[0] * m_capSize[1]);
                m_avail = true;
              }
              else if (!m_pyfilter) {
                std::memcpy(m_viewportImage, color_buffer, 4 * m_capSize[0] * m_capSize[1]);
                FilterRGBA32 filt;
                filterImage(filt, m_viewportImage, m_capSize);
              }
              else {
                std::memcpy(m_viewportImage, color_buffer, 4 * m_capSize[0] * m_capSize[1]);
                FilterRGBA32 filt;
                filterImage(filt, m_viewportImage, m_capSize);
              }
              MEM_delete(color_buffer);
            }
          }
          else {
            GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
            float *color_buffer = (float *)GPU_texture_read(
                GPU_framebuffer_color_texture(target), GPU_DATA_FLOAT, 0);

            if (color_buffer) {
              std::memcpy(m_viewportImage, color_buffer, 3 * m_capSize[0] * m_capSize[1]);

              FilterRGB24 filt;
              filterImage(filt, m_viewportImage, m_capSize);

              MEM_delete(color_buffer);
            }
          }
        }
      }
    }
  }
}

bool ImageViewport::loadImage(unsigned int *buffer, unsigned int size, double ts)
{
  unsigned int *tmp_image;
  bool ret;

  // if scale was changed
  if (m_scaleChange) {
    // reset image
    init(m_capSize[0], m_capSize[1]);
  }
  // size must be identical
  if (size < getBuffSize())
    return false;

  if (m_avail) {
    // just copy
    return ImageBase::loadImage(buffer, size, ts);
  }
  else {
    tmp_image = m_image;
    m_image = buffer;
    calcViewport(0, ts);
    ret = m_avail;
    m_image = tmp_image;
    // since the image was not loaded to our buffer, it's not valid
    m_avail = false;
  }
  return ret;
}

// cast Image pointer to ImageViewport
inline ImageViewport *getImageViewport(PyImage *self)
{
  return static_cast<ImageViewport *>(self->m_image);
}

// python methods

// get whole
PyObject *ImageViewport_getWhole(PyImage *self, void *closure)
{
  if (self->m_image != nullptr && getImageViewport(self)->getWhole())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set whole
int ImageViewport_setWhole(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  try {
    // set whole, can throw in case of resize and buffer exports
    if (self->m_image != nullptr)
      getImageViewport(self)->setWhole(value == Py_True);
  }
  catch (Exception &exp) {
    exp.report();
    return -1;
  }
  // success
  return 0;
}

// get alpha
PyObject *ImageViewport_getAlpha(PyImage *self, void *closure)
{
  if (self->m_image != nullptr && getImageViewport(self)->getAlpha())
    Py_RETURN_TRUE;
  else
    Py_RETURN_FALSE;
}

// set whole
int ImageViewport_setAlpha(PyImage *self, PyObject *value, void *closure)
{
  // check parameter, report failure
  if (value == nullptr || !PyBool_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "The value must be a bool");
    return -1;
  }
  // set alpha
  if (self->m_image != nullptr)
    getImageViewport(self)->setAlpha(value == Py_True);
  // success
  return 0;
}

// get position
static PyObject *ImageViewport_getPosition(PyImage *self, void *closure)
{
  int *pos = getImageViewport(self)->getPosition();
  PyObject *ret = PyTuple_New(2);
  PyTuple_SET_ITEM(ret, 0, PyLong_FromLong(pos[0]));
  PyTuple_SET_ITEM(ret, 1, PyLong_FromLong(pos[1]));
  return ret;
}

// set position
static int ImageViewport_setPosition(PyImage *self, PyObject *value, void *closure)
{
  // check validity of parameter
  if (value == nullptr || !(PyTuple_Check(value) || PyList_Check(value)) ||
      PySequence_Fast_GET_SIZE(value) != 2 || !PyLong_Check(PySequence_Fast_GET_ITEM(value, 0)) ||
      !PyLong_Check(PySequence_Fast_GET_ITEM(value, 1))) {
    PyErr_SetString(PyExc_TypeError, "The value must be a sequence of 2 ints");
    return -1;
  }
  // set position
  int pos[2] = {int(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 0))),
                  int(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 1)))};
  getImageViewport(self)->setPosition(pos);
  // success
  return 0;
}

// get capture size
PyObject *ImageViewport_getCaptureSize(PyImage *self, void *closure)
{
  short *size = getImageViewport(self)->getCaptureSize();
  PyObject *ret = PyTuple_New(2);
  PyTuple_SET_ITEM(ret, 0, PyLong_FromLong(size[0]));
  PyTuple_SET_ITEM(ret, 1, PyLong_FromLong(size[1]));
  return ret;
}

// set capture size
int ImageViewport_setCaptureSize(PyImage *self, PyObject *value, void *closure)
{
  // check validity of parameter
  if (value == nullptr || !(PyTuple_Check(value) || PyList_Check(value)) ||
      PySequence_Fast_GET_SIZE(value) != 2 || !PyLong_Check(PySequence_Fast_GET_ITEM(value, 0)) ||
      !PyLong_Check(PySequence_Fast_GET_ITEM(value, 1))) {
    PyErr_SetString(PyExc_TypeError, "The value must be a sequence of 2 ints");
    return -1;
  }
  // set capture size
  short size[2] = {short(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 0))),
                   short(PyLong_AsLong(PySequence_Fast_GET_ITEM(value, 1)))};
  try {
    // can throw in case of resize and buffer exports
    getImageViewport(self)->setCaptureSize(size);
  }
  catch (Exception &exp) {
    exp.report();
    return -1;
  }
  // success
  return 0;
}

// methods structure
static PyMethodDef imageViewportMethods[] = {  // methods from ImageBase class
    {"refresh",
     (PyCFunction)Image_refresh,
     METH_VARARGS,
     "Refresh image - invalidate its current content"},
    {nullptr}};
// attributes structure
static PyGetSetDef imageViewportGetSets[] = {
    {(char *)"whole",
     (getter)ImageViewport_getWhole,
     (setter)ImageViewport_setWhole,
     (char *)"use whole viewport to capture",
     nullptr},
    {(char *)"position",
     (getter)ImageViewport_getPosition,
     (setter)ImageViewport_setPosition,
     (char *)"upper left corner of captured area",
     nullptr},
    {(char *)"capsize",
     (getter)ImageViewport_getCaptureSize,
     (setter)ImageViewport_setCaptureSize,
     (char *)"size of viewport area being captured",
     nullptr},
    {(char *)"alpha",
     (getter)ImageViewport_getAlpha,
     (setter)ImageViewport_setAlpha,
     (char *)"use alpha in texture",
     nullptr},
    // attributes from ImageBase class
    {(char *)"valid",
     (getter)Image_valid,
     nullptr,
     (char *)"bool to tell if an image is available",
     nullptr},
    {(char *)"image", (getter)Image_getImage, nullptr, (char *)"image data", nullptr},
    {(char *)"size", (getter)Image_getSize, nullptr, (char *)"image size", nullptr},
    {(char *)"scale",
     (getter)Image_getScale,
     (setter)Image_setScale,
     (char *)"fast scale of image (near neighbor)",
     nullptr},
    {(char *)"flip",
     (getter)Image_getFlip,
     (setter)Image_setFlip,
     (char *)"flip image vertically",
     nullptr},
    {(char *)"zbuff",
     (getter)Image_getZbuff,
     (setter)Image_setZbuff,
     (char *)"use depth buffer as texture",
     nullptr},
    {(char *)"depth",
     (getter)Image_getDepth,
     (setter)Image_setDepth,
     (char *)"get depth information from z-buffer as array of float",
     nullptr},
    {(char *)"filter",
     (getter)Image_getFilter,
     (setter)Image_setFilter,
     (char *)"pixel filter",
     nullptr},
    {nullptr}};

// define python type
PyTypeObject ImageViewportType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "VideoTexture.ImageViewport", /*tp_name*/
    sizeof(PyImage),                                                /*tp_basicsize*/
    0,                                                              /*tp_itemsize*/
    (destructor)Image_dealloc,                                      /*tp_dealloc*/
    0,                                                              /*tp_print*/
    0,                                                              /*tp_getattr*/
    0,                                                              /*tp_setattr*/
    0,                                                              /*tp_compare*/
    0,                                                              /*tp_repr*/
    0,                                                              /*tp_as_number*/
    0,                                                              /*tp_as_sequence*/
    0,                                                              /*tp_as_mapping*/
    0,                                                              /*tp_hash */
    0,                                                              /*tp_call*/
    0,                                                              /*tp_str*/
    0,                                                              /*tp_getattro*/
    0,                                                              /*tp_setattro*/
    &imageBufferProcs,                                              /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,                                             /*tp_flags*/
    "Image source from viewport",                                   /* tp_doc */
    0,                                                              /* tp_traverse */
    0,                                                              /* tp_clear */
    0,                                                              /* tp_richcompare */
    0,                                                              /* tp_weaklistoffset */
    0,                                                              /* tp_iter */
    0,                                                              /* tp_iternext */
    imageViewportMethods,                                           /* tp_methods */
    0,                                                              /* tp_members */
    imageViewportGetSets,                                           /* tp_getset */
    0,                                                              /* tp_base */
    0,                                                              /* tp_dict */
    0,                                                              /* tp_descr_get */
    0,                                                              /* tp_descr_set */
    0,                                                              /* tp_dictoffset */
    (initproc)Image_init<ImageViewport>,                            /* tp_init */
    0,                                                              /* tp_alloc */
    Image_allocNew,                                                 /* tp_new */
};
