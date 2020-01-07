#include "exporter.h"

#include "render/colormanager.h"
#include "render/pixelservice.h"

Exporter::Exporter(ViewerOutput* viewer,
                   Encoder *encoder,
                   QObject* parent) :
  QObject(parent),
  video_backend_(nullptr),
  audio_backend_(nullptr),
  viewer_node_(viewer),
  video_done_(true),
  audio_done_(true),
  encoder_(encoder),
  export_status_(false),
  export_msg_(tr("Export hasn't started yet"))
{
  connect(this, &Exporter::ExportEnded, this, &Exporter::deleteLater);
}

void Exporter::EnableVideo(const VideoRenderingParams &video_params, const QMatrix4x4 &transform, ColorProcessorPtr color_processor)
{
  video_params_ = video_params;
  transform_ = transform;
  color_processor_ = color_processor;

  video_done_ = false;
}

void Exporter::EnableAudio(const AudioRenderingParams &audio_params)
{
  audio_params_ = audio_params;

  audio_done_ = false;
}

bool Exporter::GetExportStatus() const
{
  return export_status_;
}

const QString &Exporter::GetExportError() const
{
  return export_msg_;
}

void Exporter::StartExporting()
{
  // Default to error state until ExportEnd is called
  export_status_ = false;

  // Create renderers
  if (!Initialize()) {
    SetExportMessage("Failed to initialize exporter");
    ExportFailed();
    return;
  }

  // Create renderers
  if (!video_done_) {
    video_backend_->SetViewerNode(viewer_node_);
    video_backend_->SetParameters(VideoRenderingParams(viewer_node_->video_params().width(),
                                                       viewer_node_->video_params().height(),
                                                       video_params_.time_base(),
                                                       video_params_.format(),
                                                       video_params_.mode()));

    waiting_for_frame_ = 0;
  }

  if (!audio_done_) {
    audio_backend_->SetViewerNode(viewer_node_);
    audio_backend_->SetParameters(audio_params_);
  }

  // Open encoder and wait for result
  connect(encoder_, &Encoder::OpenSucceeded, this, &Exporter::EncoderOpenedSuccessfully, Qt::QueuedConnection);
  connect(encoder_, &Encoder::OpenFailed, this, &Exporter::EncoderOpenFailed, Qt::QueuedConnection);
  connect(encoder_, &Encoder::AudioComplete, this, &Exporter::AudioEncodeComplete, Qt::QueuedConnection);
  connect(encoder_, &Encoder::Closed, encoder_, &Encoder::deleteLater, Qt::QueuedConnection);

  QMetaObject::invokeMethod(encoder_,
                            "Open",
                            Qt::QueuedConnection);
}

void Exporter::SetExportMessage(const QString &s)
{
  export_msg_ = s;
}

void Exporter::ExportSucceeded()
{
  if (!audio_done_ || !video_done_) {
    return;
  }

  Cleanup();

  if (video_backend_) {
    video_backend_->deleteLater();
  }

  export_status_ = true;

  connect(encoder_, &Encoder::Closed, this, &Exporter::EncoderClosed);

  QMetaObject::invokeMethod(encoder_,
                            "Close",
                            Qt::QueuedConnection);
}

void Exporter::ExportFailed()
{
  emit ExportEnded();
}

void Exporter::EncodeFrame(const rational &time, QVariant value)
{
  if (time == waiting_for_frame_) {
    bool get_cached = false;

    do {
      if (get_cached) {
        value = cached_frames_.take(waiting_for_frame_);
      } else {
        get_cached = true;
      }

      // Convert texture to frame
      FramePtr frame = TextureToFrame(value);

      // OCIO conversion requires a frame in 32F format
      if (frame->format() != PixelFormat::PIX_FMT_RGBA32F) {
        frame = PixelService::ConvertPixelFormat(frame, PixelFormat::PIX_FMT_RGBA32F);
      }

      // Color conversion must be done with unassociated alpha, and the pipeline is always associated
      ColorManager::DisassociateAlpha(frame);

      // Convert color space
      color_processor_->ConvertFrame(frame);

      // Set frame timestamp
      frame->set_timestamp(waiting_for_frame_);

      // Encode (may require re-associating alpha?)
      QMetaObject::invokeMethod(encoder_,
                                "WriteFrame",
                                Qt::QueuedConnection,
                                Q_ARG(FramePtr, frame));

      waiting_for_frame_ += video_params_.time_base();

      // Calculate progress
      int progress = qRound(100.0 * (waiting_for_frame_.toDouble() / viewer_node_->Length().toDouble()));
      emit ProgressChanged(progress);

    } while (cached_frames_.contains(waiting_for_frame_));

    if (waiting_for_frame_ >= viewer_node_->Length()) {
      video_done_ = true;

      ExportSucceeded();
    }
  } else {
    cached_frames_.insert(time, value);
  }
}

void Exporter::FrameRendered(const rational &time, QVariant value)
{
  qDebug() << "Received" << time.toDouble() << "- waiting for" << waiting_for_frame_.toDouble();

  const QMap<rational, QByteArray>& time_hash_map = video_backend_->frame_cache()->time_hash_map();

  QByteArray map_hash = time_hash_map.value(time);

  EncodeFrame(time, value);

  QList<rational> matching_times = matched_frames_.value(map_hash);
  foreach (const rational& t, matching_times) {
    qDebug() << "    Also matches" << t;
    EncodeFrame(t, value);
  }
}

void Exporter::AudioRendered()
{
  // Retrieve the audio filename
  QString cache_fn = audio_backend_->CachePathName();

  QMetaObject::invokeMethod(encoder_,
                            "WriteAudio",
                            Qt::QueuedConnection,
                            Q_ARG(const AudioRenderingParams&, audio_backend_->params()),
                            Q_ARG(const QString&, cache_fn));

  // We don't need the audio backend anymore
  audio_backend_->deleteLater();
}

void Exporter::AudioEncodeComplete()
{
  audio_done_ = true;

  ExportSucceeded();
}

void Exporter::EncoderOpenedSuccessfully()
{
  // Invalidate caches
  if (!video_done_) {
    // First we generate the hashes so we know exactly how many frames we need
    video_backend_->SetOperatingMode(VideoRenderWorker::kHashOnly);
    connect(video_backend_, &VideoRenderBackend::QueueComplete, this, &Exporter::VideoHashesComplete);

    video_backend_->InvalidateCache(0, viewer_node_->Length());
  }

  if (!audio_done_) {
    // We set the audio backend to render the full sequence to the disk
    connect(audio_backend_, &AudioRenderBackend::QueueComplete, this, &Exporter::AudioRendered);

    audio_backend_->InvalidateCache(0, viewer_node_->Length());
  }
}

void Exporter::EncoderOpenFailed()
{
  SetExportMessage("Failed to open encoder");
  ExportFailed();
}

void Exporter::EncoderClosed()
{
  emit ProgressChanged(100);
  emit ExportEnded();
}

void Exporter::VideoHashesComplete()
{
  // We've got our hashes, time to kick off actual rendering
  disconnect(video_backend_, &VideoRenderBackend::QueueComplete, this, &Exporter::VideoHashesComplete);

  // Determine what frames will be hashed
  TimeRangeList ranges;
  ranges.append(TimeRange(0, viewer_node_->Length()));

  QMap<rational, QByteArray>::const_iterator i;
  QMap<rational, QByteArray>::iterator j;

  // Copy time hash map
  QMap<rational, QByteArray> time_hash_map = video_backend_->frame_cache()->time_hash_map();

  // Check for any times that share duplicate hashes
  for (i=time_hash_map.begin();i!=time_hash_map.end();i++) {
    j = time_hash_map.begin();

    while (j != time_hash_map.end()) {
      if (i != j && i.value() == j.value()) {
        // Remove the time range from this and
        ranges.RemoveTimeRange(TimeRange(j.key(), j.key() + video_backend_->params().time_base()));

        QList<rational> times_with_this_hash = matched_frames_.value(i.value());
        times_with_this_hash.append(j.key());
        matched_frames_.insert(i.value(), times_with_this_hash);

        j = time_hash_map.erase(j);
      } else {
        j++;
      }
    }
  }

  // Set video backend to render mode but NOT hash or download
  video_backend_->SetOperatingMode(VideoRenderWorker::kRenderOnly);
  video_backend_->SetOnlySignalLastFrameRequested(false);

  connect(video_backend_, &VideoRenderBackend::CachedFrameReady, this, &Exporter::FrameRendered);

  foreach (const TimeRange& range, ranges) {
    video_backend_->InvalidateCache(range.in(), range.out());
  }
}
