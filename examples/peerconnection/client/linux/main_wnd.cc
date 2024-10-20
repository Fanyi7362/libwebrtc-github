/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/linux/main_wnd.h"

#include <cairo.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <glib-object.h>
#include <glib.h>
#include <gobject/gclosure.h>
#include <gtk/gtk.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>
#include <sys/stat.h>
#include <cinttypes>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "api/video/video_rotation.h"
#include "api/video/video_source_interface.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"
#include "third_party/libyuv/include/libyuv.h"
#include "api/environment/environment_factory.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "modules/video_coding/include/video_codec_interface.h"



namespace {

//
// Simple static functions that simply forward the callback to the
// GtkMainWnd instance.
//

gboolean OnDestroyedCallback(GtkWidget* widget,
                             GdkEvent* event,
                             gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnDestroyed(widget, event);
  return FALSE;
}

void OnClickedCallback(GtkWidget* widget, gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnClicked(widget);
}

gboolean SimulateButtonClick(gpointer button) {
  g_signal_emit_by_name(button, "clicked");
  return false;
}

gboolean OnKeyPressCallback(GtkWidget* widget,
                            GdkEventKey* key,
                            gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnKeyPress(widget, key);
  return false;
}

void OnRowActivatedCallback(GtkTreeView* tree_view,
                            GtkTreePath* path,
                            GtkTreeViewColumn* column,
                            gpointer data) {
  reinterpret_cast<GtkMainWnd*>(data)->OnRowActivated(tree_view, path, column);
}

gboolean SimulateLastRowActivated(gpointer data) {
  GtkTreeView* tree_view = reinterpret_cast<GtkTreeView*>(data);
  GtkTreeModel* model = gtk_tree_view_get_model(tree_view);

  // "if iter is NULL, then the number of toplevel nodes is returned."
  int rows = gtk_tree_model_iter_n_children(model, NULL);
  GtkTreePath* lastpath = gtk_tree_path_new_from_indices(rows - 1, -1);

  // Select the last item in the list
  GtkTreeSelection* selection = gtk_tree_view_get_selection(tree_view);
  gtk_tree_selection_select_path(selection, lastpath);

  // Our TreeView only has one column, so it is column 0.
  GtkTreeViewColumn* column = gtk_tree_view_get_column(tree_view, 0);

  gtk_tree_view_row_activated(tree_view, lastpath, column);

  gtk_tree_path_free(lastpath);
  return false;
}

// Creates a tree view, that we use to display the list of peers.
void InitializeList(GtkWidget* list) {
  GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes(
      "List Items", renderer, "text", 0, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(list), column);
  GtkListStore* store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_INT);
  gtk_tree_view_set_model(GTK_TREE_VIEW(list), GTK_TREE_MODEL(store));
  g_object_unref(store);
}

// Adds an entry to a tree view.
void AddToList(GtkWidget* list, const gchar* str, int value) {
  GtkListStore* store =
      GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(list)));

  GtkTreeIter iter;
  gtk_list_store_append(store, &iter);
  gtk_list_store_set(store, &iter, 0, str, 1, value, -1);
}

struct UIThreadCallbackData {
  explicit UIThreadCallbackData(MainWndCallback* cb, int id, void* d)
      : callback(cb), msg_id(id), data(d) {}
  MainWndCallback* callback;
  int msg_id;
  void* data;
};

gboolean HandleUIThreadCallback(gpointer data) {
  UIThreadCallbackData* cb_data = reinterpret_cast<UIThreadCallbackData*>(data);
  cb_data->callback->UIThreadCallback(cb_data->msg_id, cb_data->data);
  delete cb_data;
  return false;
}

gboolean Redraw(gpointer data) {
  GtkMainWnd* wnd = reinterpret_cast<GtkMainWnd*>(data);
  wnd->OnRedraw();
  return false;
}

gboolean Draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
  GtkMainWnd* wnd = reinterpret_cast<GtkMainWnd*>(data);
  wnd->Draw(widget, cr);
  return false;
}

}  // namespace

//
// GtkMainWnd implementation.
//

GtkMainWnd::GtkMainWnd(const char* server,
                       int port,
                       bool autoconnect,
                       bool autocall,
                       bool disable_gui)
    : window_(NULL),
      draw_area_(NULL),
      vbox_(NULL),
      server_edit_(NULL),
      port_edit_(NULL),
      peer_list_(NULL),
      callback_(NULL),
      server_(server),
      autoconnect_(autoconnect),
      autocall_(autocall),
      disable_gui_(disable_gui) {
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%i", port);
  port_ = buffer;
}

GtkMainWnd::~GtkMainWnd() {
  RTC_DCHECK(!IsWindow());
}

void GtkMainWnd::RegisterObserver(MainWndCallback* callback) {
  callback_ = callback;
}

bool GtkMainWnd::IsWindow() {
  return window_ != NULL && GTK_IS_WINDOW(window_);
}

void GtkMainWnd::MessageBox(const char* caption,
                            const char* text,
                            bool is_error) {
  GtkWidget* dialog = gtk_message_dialog_new(
      GTK_WINDOW(window_), GTK_DIALOG_DESTROY_WITH_PARENT,
      is_error ? GTK_MESSAGE_ERROR : GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s",
      text);
  gtk_window_set_title(GTK_WINDOW(dialog), caption);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

MainWindow::UI GtkMainWnd::current_ui() {
  if (vbox_)
    return CONNECT_TO_SERVER;

  if (peer_list_)
    return LIST_PEERS;

  return STREAMING;
}

void GtkMainWnd::StartLocalRenderer(webrtc::VideoTrackInterface* local_video, int my_id) {
  local_renderer_.reset(new VideoRenderer(this, local_video, my_id, false));
}

void GtkMainWnd::StopLocalRenderer() {
  local_renderer_.reset();
}

// In this function, remote_renderer_ from MainWnd will be added to WebRTC 
// to receive and render remote video frames.
void GtkMainWnd::StartRemoteRenderer(
    webrtc::VideoTrackInterface* remote_video, int my_id) {
  // Generate a remote video renderer and register it with WebRTC.
  remote_renderer_.reset(new VideoRenderer(this, remote_video, my_id, true));
}

void GtkMainWnd::StopRemoteRenderer() {
  remote_renderer_.reset();
}

// HandleUIThreadCallback points to Conductor::UIThreadCallback
// adding a task to this message queue
void GtkMainWnd::QueueUIThreadCallback(int msg_id, void* data) {
  if (disable_gui_) {
    // In non-GUI mode, directly push the callback to the conductor's pending messages queue.
    callback_->QueuePendingMessage(msg_id, data);
  } else {
    // In GUI mode, use GTK's g_idle_add to handle the callback.
    g_idle_add(HandleUIThreadCallback, new UIThreadCallbackData(callback_, msg_id, data));
  }
}

bool GtkMainWnd::Create() {
  if (disable_gui_) {
    return true;  // Skip GUI creation in automatic mode
  }  
  RTC_DCHECK(window_ == NULL);

  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  if (window_) {
    gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(window_), 640, 480);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);  // Make window non-resizable
    gtk_window_set_title(GTK_WINDOW(window_), "PeerConnection client");
    g_signal_connect(G_OBJECT(window_), "delete-event",
                     G_CALLBACK(&OnDestroyedCallback), this);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(OnKeyPressCallback),
                     this);

    SwitchToConnectUI();
  }

  return window_ != NULL;
}

bool GtkMainWnd::Destroy() {
  if (!IsWindow())
    return false;

  gtk_widget_destroy(window_);
  window_ = NULL;

  return true;
}

void GtkMainWnd::SwitchToConnectUI() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (disable_gui_) {
    // Automatically connect in GUI-less mode
    callback_->StartLogin(server_, atoi(port_.c_str()));
    return;
  }  

  RTC_DCHECK(IsWindow());
  RTC_DCHECK(vbox_ == NULL);

  gtk_container_set_border_width(GTK_CONTAINER(window_), 10);

  if (peer_list_) {
    gtk_widget_destroy(peer_list_);
    peer_list_ = NULL;
  }

  vbox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  GtkWidget* valign = gtk_alignment_new(0, 1, 0, 0);
  gtk_container_add(GTK_CONTAINER(vbox_), valign);
  gtk_container_add(GTK_CONTAINER(window_), vbox_);

  GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  GtkWidget* label = gtk_label_new("Server");
  gtk_container_add(GTK_CONTAINER(hbox), label);

  server_edit_ = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(server_edit_), server_.c_str());
  gtk_widget_set_size_request(server_edit_, 400, 30);
  gtk_container_add(GTK_CONTAINER(hbox), server_edit_);

  port_edit_ = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(port_edit_), port_.c_str());
  gtk_widget_set_size_request(port_edit_, 70, 30);
  gtk_container_add(GTK_CONTAINER(hbox), port_edit_);

  GtkWidget* button = gtk_button_new_with_label("Connect");
  gtk_widget_set_size_request(button, 70, 30);
  g_signal_connect(button, "clicked", G_CALLBACK(OnClickedCallback), this);
  gtk_container_add(GTK_CONTAINER(hbox), button);

  GtkWidget* halign = gtk_alignment_new(1, 0, 0, 0);
  gtk_container_add(GTK_CONTAINER(halign), hbox);
  gtk_box_pack_start(GTK_BOX(vbox_), halign, FALSE, FALSE, 0);

  gtk_widget_show_all(window_);

  if (autoconnect_)
    g_idle_add(SimulateButtonClick, button);
}

void GtkMainWnd::SwitchToPeerList(const Peers& peers) {
  RTC_LOG(LS_INFO) << __FUNCTION__;  
  if (disable_gui_) {
    RTC_LOG(LS_INFO) << "GUI disabled, skipping UI switch";
    return;
  }  

  if (!peer_list_) {
    gtk_container_set_border_width(GTK_CONTAINER(window_), 0);
    if (vbox_) {
      gtk_widget_destroy(vbox_);
      vbox_ = NULL;
      server_edit_ = NULL;
      port_edit_ = NULL;
    } else if (draw_area_) {
      gtk_widget_destroy(draw_area_);
      draw_area_ = NULL;
      draw_buffer_.reset();
    }

    // wait for enter key press, or row click
    peer_list_ = gtk_tree_view_new();
    g_signal_connect(peer_list_, "row-activated",
                     G_CALLBACK(OnRowActivatedCallback), this);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(peer_list_), FALSE);
    InitializeList(peer_list_);
    gtk_container_add(GTK_CONTAINER(window_), peer_list_);
    gtk_widget_show_all(window_);
  } else {
    GtkListStore* store =
        GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(peer_list_)));
    gtk_list_store_clear(store);
  }

  AddToList(peer_list_, "List of currently connected peers:", -1);
  for (Peers::const_iterator i = peers.begin(); i != peers.end(); ++i)
    AddToList(peer_list_, i->second.c_str(), i->first);

  if (autocall_ && peers.begin() != peers.end())
    g_idle_add(SimulateLastRowActivated, peer_list_);
}

void GtkMainWnd::SwitchToStreamingUI() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (disable_gui_) {
    RTC_LOG(LS_INFO) << "GUI disabled, skipping UI switch";
    return;
  }

  RTC_DCHECK(draw_area_ == NULL);

  gtk_container_set_border_width(GTK_CONTAINER(window_), 0);
  if (peer_list_) {
    gtk_widget_destroy(peer_list_);
    peer_list_ = NULL;
  }

  draw_area_ = gtk_drawing_area_new();
  gtk_container_add(GTK_CONTAINER(window_), draw_area_);
  g_signal_connect(G_OBJECT(draw_area_), "draw", G_CALLBACK(&::Draw), this);

  gtk_widget_show_all(window_);
}

void GtkMainWnd::OnDestroyed(GtkWidget* widget, GdkEvent* event) {
  callback_->Close();
  window_ = NULL;
  draw_area_ = NULL;
  vbox_ = NULL;
  server_edit_ = NULL;
  port_edit_ = NULL;
  peer_list_ = NULL;
}

void GtkMainWnd::OnClicked(GtkWidget* widget) {
  // Make the connect button insensitive, so that it cannot be clicked more than
  // once.  Now that the connection includes auto-retry, it should not be
  // necessary to click it more than once.
  gtk_widget_set_sensitive(widget, false);
  server_ = gtk_entry_get_text(GTK_ENTRY(server_edit_));
  port_ = gtk_entry_get_text(GTK_ENTRY(port_edit_));
  int port = port_.length() ? atoi(port_.c_str()) : 0;
  callback_->StartLogin(server_, port);
}

void GtkMainWnd::OnKeyPress(GtkWidget* widget, GdkEventKey* key) {
  if (key->type == GDK_KEY_PRESS) {
    switch (key->keyval) {
      case GDK_KEY_Escape:
        if (draw_area_) {
          callback_->DisconnectFromCurrentPeer();
        } else if (peer_list_) {
          callback_->DisconnectFromServer();
        }
        break;

      case GDK_KEY_KP_Enter:
      case GDK_KEY_Return:
        if (vbox_) {
          OnClicked(NULL);
        } else if (peer_list_) {
          // OnRowActivated will be called automatically when the user
          // presses enter.
        }
        break;

      default:
        break;
    }
  }
}

void GtkMainWnd::OnRowActivated(GtkTreeView* tree_view,
                                GtkTreePath* path,
                                GtkTreeViewColumn* column) {
  RTC_DCHECK(peer_list_ != NULL);
  GtkTreeIter iter;
  GtkTreeModel* model;
  GtkTreeSelection* selection =
      gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_view));
  if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
    char* text;
    int id = -1;
    // get the clicked peer_id
    gtk_tree_model_get(model, &iter, 0, &text, 1, &id, -1);
    if (id != -1)
      callback_->ConnectToPeer(id); // call the Conductor::ConnectToPeer
    g_free(text);
  }
}


void GtkMainWnd::OnRedraw() {
  gdk_threads_enter();

  VideoRenderer* remote_renderer = remote_renderer_.get();
  if (remote_renderer && remote_renderer->image() != NULL &&
      draw_area_ != NULL) {
    int window_width, window_height;
    gtk_window_get_size(GTK_WINDOW(window_), &window_width, &window_height);

    // Calculate scaling factors
    float scale_x = static_cast<float>(window_width) / remote_renderer->width();
    float scale_y = static_cast<float>(window_height) / remote_renderer->height();
    float scale = std::min(scale_x, scale_y);  // Maintain aspect ratio

    int scaled_width = static_cast<int>(remote_renderer->width() * scale);
    int scaled_height = static_cast<int>(remote_renderer->height() * scale);

    if (!draw_buffer_.get() || scaled_width != width_ || scaled_height != height_) {
      width_ = scaled_width;
      height_ = scaled_height;
      draw_buffer_.reset(new uint8_t[width_ * height_ * 4]);
      gtk_widget_set_size_request(draw_area_, width_, height_);
    }

    const uint32_t* image = reinterpret_cast<const uint32_t*>(remote_renderer->image());
    uint32_t* scaled = reinterpret_cast<uint32_t*>(draw_buffer_.get());

    // Scale the image
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        int src_x = static_cast<int>(x / scale);
        int src_y = static_cast<int>(y / scale);
        scaled[y * width_ + x] = image[src_y * remote_renderer->width() + src_x];
      }
    }

    // Handle local renderer scaling similarly...

    gtk_widget_queue_draw(draw_area_);
  }

  gdk_threads_leave();
}


void GtkMainWnd::Draw(GtkWidget* widget, cairo_t* cr) {
  int window_width, window_height;
  gtk_window_get_size(GTK_WINDOW(window_), &window_width, &window_height);

  int x_offset = (window_width - width_) / 2;
  int y_offset = (window_height - height_) / 2;

  cairo_set_source_rgb(cr, 0, 0, 0);  // Set background color to black
  cairo_paint(cr);  // Fill the entire window with the background color

  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      draw_buffer_.get(), CAIRO_FORMAT_RGB24, width_, height_, width_ * 4);
  cairo_set_source_surface(cr, surface, x_offset, y_offset);
  cairo_paint(cr);
  cairo_surface_destroy(surface);
}


GtkMainWnd::VideoRenderer::VideoRenderer(
    GtkMainWnd* main_wnd,
    webrtc::VideoTrackInterface* track_to_render, 
    int peer_id,
    bool save_enabled)
    : width_(0),
      height_(0),
      main_wnd_(main_wnd),
      rendered_track_(track_to_render),
      peer_id_(peer_id), 
      video_file_(nullptr),
      metadata_file_(nullptr),
      frame_count_(0),
      save_enabled_(save_enabled),
      first_frame_timestamp_us_(0),
      first_rtp_timestamp_(0),
      target_width_(1280),
      target_height_(720) {
  rendered_track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
  if (save_enabled_) {
    if (!InitializeVideoFile()) {
      RTC_LOG(LS_ERROR) << "Failed to initialize video file";
      save_enabled_ = false;
    }
  }

  time_base_ = 1.0 / 90000.0; 

  // Initialize encoder
  webrtc::VideoEncoderFactoryTemplate<webrtc::LibvpxVp8EncoderTemplateAdapter> factory;
  
  // Create a default Environment
  webrtc::EnvironmentFactory env_factory;
  webrtc::Environment env = env_factory.Create();

  encoder_ = factory.Create(env, webrtc::SdpVideoFormat("VP8"));
  
  // Configure codec settings
  codec_settings_.codecType = webrtc::kVideoCodecVP8;
  codec_settings_.width = target_width_;
  codec_settings_.height = target_height_;
  codec_settings_.startBitrate = 2000;  // kbps
  codec_settings_.maxBitrate = 4000;
  codec_settings_.maxFramerate = 30;
  codec_settings_.VP8()->denoisingOn = true;
  codec_settings_.VP8()->automaticResizeOn = false;  // We handle resizing ourselves
  codec_settings_.VP8()->keyFrameInterval = 30;

  encoder_->InitEncode(&codec_settings_, webrtc::VideoEncoder::Settings(
      webrtc::VideoEncoder::Capabilities(false), 1, 0));
  encoder_->RegisterEncodeCompleteCallback(this);
}

GtkMainWnd::VideoRenderer::~VideoRenderer() {
  rendered_track_->RemoveSink(this);
  if (save_enabled_) {
    CloseVideoFile();
  }
}

void GtkMainWnd::VideoRenderer::SetSize(int width, int height) {
  gdk_threads_enter();

  if (width_ == width && height_ == height) {
    gdk_threads_leave();
    return;
  }

  RTC_LOG(LS_INFO) << "Video size changed: " << width_ << "x" << height_ << " -> " << width << "x" << height;  

  width_ = width;
  height_ = height;
  image_.reset(new uint8_t[width * height * 4]); 

  gdk_threads_leave();
}

void GtkMainWnd::VideoRenderer::OnFrame(const webrtc::VideoFrame& video_frame) {
  gdk_threads_enter();

  rtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
      video_frame.video_frame_buffer()->ToI420());
  if (video_frame.rotation() != webrtc::kVideoRotation_0) {
    buffer = webrtc::I420Buffer::Rotate(*buffer, video_frame.rotation());
  }
  SetSize(buffer->width(), buffer->height());

  libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                     buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                     image_.get(), width_ * 4, buffer->width(),
                     buffer->height());

  if (save_enabled_) {
    EncodeAndSaveFrame(video_frame);
  }

  gdk_threads_leave();

  g_idle_add(Redraw, main_wnd_);
}

bool GtkMainWnd::VideoRenderer::InitializeVideoFile() {
  std::string video_filename = GetOutputFilename(".ivf");
  video_file_ = fopen(video_filename.c_str(), "wb");
  if (!video_file_) {
    RTC_LOG(LS_ERROR) << "Could not open video file for writing: " << video_filename;
    return false;
  }

  // Write IVF file header
  WriteIvfFileHeader();

  std::string metadata_filename = GetOutputFilename(".meta");
  metadata_file_ = fopen(metadata_filename.c_str(), "w");
  if (!metadata_file_) {
    RTC_LOG(LS_ERROR) << "Could not open metadata file for writing: " << metadata_filename;
    fclose(video_file_);
    video_file_ = nullptr;
    return false;
  }

  // Write metadata header
  fprintf(metadata_file_, "frame_number,timestamp_ms,width,height,encoded_size,is_key_frame\n");

  return true;
}


void GtkMainWnd::VideoRenderer::WriteIvfFileHeader() {
    char ivf_header[32];
    memcpy(ivf_header, "DKIF", 4);  // File signature
    uint16_t version = 0;
    memcpy(ivf_header + 4, &version, 2);
    uint16_t header_size = 32;
    memcpy(ivf_header + 6, &header_size, 2);
    memcpy(ivf_header + 8, "VP80", 4);  // FourCC
    uint16_t width = static_cast<uint16_t>(target_width_);
    uint16_t height = static_cast<uint16_t>(target_height_);
    memcpy(ivf_header + 12, &width, 2);
    memcpy(ivf_header + 14, &height, 2);
    uint32_t time_base_denominator = 90000;  // 90kHz clock rate
    uint32_t time_base_numerator = 1;  // We need to specify this
    memcpy(ivf_header + 16, &time_base_denominator, 4);
    memcpy(ivf_header + 20, &time_base_numerator, 4);
    uint32_t num_frames = 0;  // Will be updated after writing all frames
    memcpy(ivf_header + 24, &num_frames, 4);
    uint32_t reserved = 0;
    memcpy(ivf_header + 28, &reserved, 4);

    fwrite(ivf_header, 1, 32, video_file_);
}

void GtkMainWnd::VideoRenderer::WriteIvfFrameHeader(size_t frame_size, uint64_t timestamp) {
    uint32_t frame_size_32 = static_cast<uint32_t>(frame_size);
    fwrite(&frame_size_32, 1, 4, video_file_);
    fwrite(&timestamp, 1, 8, video_file_);
}


void GtkMainWnd::VideoRenderer::CloseVideoFile() {
  if (video_file_) {
    fclose(video_file_);
    video_file_ = nullptr;
  }
  if (metadata_file_) {
    fclose(metadata_file_);
    metadata_file_ = nullptr;
  }
}

bool GtkMainWnd::VideoRenderer::EncodeAndSaveFrame(const webrtc::VideoFrame& frame) {
  if (!video_file_ || !metadata_file_ || !encoder_) {
    return false;
  }

  rtc::scoped_refptr<webrtc::I420BufferInterface> i420_buffer = frame.video_frame_buffer()->ToI420();

  // Scale the frame to the target size for encoding if necessary
  rtc::scoped_refptr<webrtc::I420Buffer> scaled_buffer = webrtc::I420Buffer::Create(
      target_width_, target_height_);
  scaled_buffer->ScaleFrom(*i420_buffer);

  // Create a new frame with the scaled buffer for encoding
  webrtc::VideoFrame input_frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(scaled_buffer)
      .set_timestamp_us(frame.timestamp_us())
      .set_timestamp_rtp(frame.timestamp())
      .set_rotation(frame.rotation())
      .build();

  // Encode frame
  std::vector<webrtc::VideoFrameType> frame_types;

  int32_t encode_result = encoder_->Encode(input_frame, &frame_types);
  if (encode_result != 0) {
    RTC_LOG(LS_ERROR) << "Failed to encode frame: " << encode_result;
    return false;
  }

  return true;
}

webrtc::EncodedImageCallback::Result GtkMainWnd::VideoRenderer::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo* codec_specific_info) {
    if (!video_file_ || !metadata_file_) {
        RTC_LOG(LS_ERROR) << "Video or metadata file not initialized";
        return webrtc::EncodedImageCallback::Result(
            webrtc::EncodedImageCallback::Result::ERROR_SEND_FAILED);
    }

    uint32_t rtp_timestamp = encoded_image.RtpTimestamp();

    if (first_rtp_timestamp_ == 0) {
        first_rtp_timestamp_ = rtp_timestamp;
        first_frame_timestamp_us_ = rtc::TimeMicros();
    }

    // Calculate timestamp in the 90kHz clock domain
    uint64_t timestamp = static_cast<uint64_t>(rtp_timestamp - first_rtp_timestamp_);

    // Write IVF frame header
    WriteIvfFrameHeader(encoded_image.size(), timestamp);

    // Write encoded frame data
    size_t written = fwrite(encoded_image.data(), 1, encoded_image.size(), video_file_);
    if (written != encoded_image.size()) {
        RTC_LOG(LS_ERROR) << "Error writing encoded frame data: " << written << " of " << encoded_image.size() << " bytes written";
        return webrtc::EncodedImageCallback::Result(
            webrtc::EncodedImageCallback::Result::ERROR_SEND_FAILED);
    }

    // Calculate timestamp in microseconds for metadata
    uint64_t timestamp_us = rtc::TimeMicros() - first_frame_timestamp_us_;

    // Write metadata
    fprintf(metadata_file_, "%d,%" PRIu64 ",%u,%u,%zu,%d\n", 
            frame_count_,
            timestamp_us,
            encoded_image._encodedWidth,
            encoded_image._encodedHeight,
            encoded_image.size(),
            encoded_image.FrameType() == webrtc::VideoFrameType::kVideoFrameKey ? 1 : 0);

    frame_count_++;

    return webrtc::EncodedImageCallback::Result(
        webrtc::EncodedImageCallback::Result::OK);
}


std::string GtkMainWnd::VideoRenderer::GetOutputFilename(const std::string& extension) const {
  const char* home_dir = std::getenv("HOME");
  if (!home_dir) {
    RTC_LOG(LS_ERROR) << "Unable to get HOME directory";
    return "";
  }

  std::string video_dir = std::string(home_dir) + "/video";
  mkdir(video_dir.c_str(), 0755);  // Create directory if it doesn't exist

  char filename[256];
  snprintf(filename, sizeof(filename), "%s/output%d%s", video_dir.c_str(), peer_id_, extension.c_str());
  return std::string(filename);
}