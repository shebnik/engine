// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sky/shell/ui/engine.h"

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/threading/worker_pool.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "mojo/data_pipe_utils/data_pipe_utils.h"
#include "mojo/public/cpp/application/connect.h"
#include "services/asset_bundle/asset_unpacker_job.h"
#include "services/asset_bundle/zip_asset_bundle.h"
#include "sky/engine/public/platform/sky_display_metrics.h"
#include "sky/engine/public/platform/WebInputEvent.h"
#include "sky/engine/public/web/Sky.h"
#include "sky/shell/dart/dart_library_provider_files.h"
#include "sky/shell/shell.h"
#include "sky/shell/ui/animator.h"
#include "sky/shell/ui/internals.h"
#include "sky/shell/ui/platform_impl.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"

namespace sky {
namespace shell {
namespace {

const char kSnapshotKey[] = "snapshot_blob.bin";

void Ignored(bool) {
  TRACE_EVENT_ASYNC_END0("flutter", "AssetUnpackerJobFetch", 1);
}

mojo::ScopedDataPipeConsumerHandle Fetch(const base::FilePath& path) {
  TRACE_EVENT_ASYNC_BEGIN0("flutter", "AssetUnpackerJobFetch", 1);
  mojo::DataPipe pipe;
  auto runner = base::WorkerPool::GetTaskRunner(true);
  mojo::common::CopyFromFile(base::FilePath(path), pipe.producer_handle.Pass(),
                             0, runner.get(), base::Bind(&Ignored));
  return pipe.consumer_handle.Pass();
}

PlatformImpl* g_platform_impl = nullptr;

}  // namespace

using mojo::asset_bundle::AssetUnpackerJob;
using mojo::asset_bundle::ZipAssetBundle;

Engine::Config::Config() {
}

Engine::Config::~Config() {
}

Engine::Engine(const Config& config, rasterizer::RasterizerPtr rasterizer)
    : config_(config),
      animator_(new Animator(config, rasterizer.Pass(), this)),
      binding_(this),
      activity_running_(false),
      have_surface_(false),
      weak_factory_(this) {
}

Engine::~Engine() {
}

base::WeakPtr<Engine> Engine::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void Engine::Init() {
  TRACE_EVENT0("flutter", "Engine::Init");

  DCHECK(!g_platform_impl);
  g_platform_impl = new PlatformImpl();
  blink::initialize(g_platform_impl);
  Shell::Shared().tracing_controller().SetDartInitialized();
}

std::unique_ptr<flow::LayerTree> Engine::BeginFrame(
    base::TimeTicks frame_time) {
  TRACE_EVENT0("flutter", "Engine::BeginFrame");

  if (!sky_view_)
    return nullptr;

  auto begin_time = base::TimeTicks::Now();
  std::unique_ptr<flow::LayerTree> layer_tree =
      sky_view_->BeginFrame(frame_time);
  if (layer_tree) {
    layer_tree->set_frame_size(SkISize::Make(display_metrics_.physical_width,
                                             display_metrics_.physical_height));
    layer_tree->set_construction_time(base::TimeTicks::Now() - begin_time);
  }
  return layer_tree;
}

void Engine::ConnectToEngine(mojo::InterfaceRequest<SkyEngine> request) {
  binding_.Bind(request.Pass());
}

void Engine::OnOutputSurfaceCreated(const base::Closure& gpu_continuation) {
  config_.gpu_task_runner->PostTask(FROM_HERE, gpu_continuation);
  have_surface_ = true;
  StartAnimatorIfPossible();
  if (sky_view_)
    ScheduleFrame();
}

void Engine::OnOutputSurfaceDestroyed(const base::Closure& gpu_continuation) {
  have_surface_ = false;
  StopAnimator();
  config_.gpu_task_runner->PostTask(FROM_HERE, gpu_continuation);
}

void Engine::SetServices(ServicesDataPtr services) {
  services_ = services.Pass();

  if (services_->scene_scheduler) {
    animator_->Reset();
    animator_->set_scene_scheduler(services_->scene_scheduler.Pass());
  } else {
#if defined(OS_ANDROID) || defined(OS_IOS)
    vsync::VSyncProviderPtr vsync_provider;
    if (services_->shell) {
      mojo::ConnectToService(services_->shell.get(), "mojo:vsync",
                             &vsync_provider);
    } else {
      mojo::ConnectToService(services_->services_provided_by_embedder.get(),
                             &vsync_provider);
    }
    animator_->Reset();
    animator_->set_vsync_provider(vsync_provider.Pass());
#endif
  }
}

void Engine::OnViewportMetricsChanged(ViewportMetricsPtr metrics) {
  display_metrics_.device_pixel_ratio = metrics->device_pixel_ratio;
  display_metrics_.physical_width = metrics->physical_width;
  display_metrics_.physical_height = metrics->physical_height;
  display_metrics_.physical_padding_top = metrics->physical_padding_top;
  display_metrics_.physical_padding_right = metrics->physical_padding_right;
  display_metrics_.physical_padding_bottom = metrics->physical_padding_bottom;
  display_metrics_.physical_padding_left = metrics->physical_padding_left;

  if (sky_view_)
    sky_view_->SetDisplayMetrics(display_metrics_);
}

void Engine::OnLocaleChanged(const mojo::String& language_code,
			     const mojo::String& country_code) {
  language_code_ = language_code;
  country_code_ = country_code;
  if (sky_view_)
    sky_view_->SetLocale(language_code_, country_code_);
}

void Engine::OnPointerPacket(pointer::PointerPacketPtr packet) {
  TRACE_EVENT0("flutter", "Engine::OnPointerPacket");

  // Convert the pointers' x and y coordinates to logical pixels.
  for (auto it = packet->pointers.begin(); it != packet->pointers.end(); ++it) {
    (*it)->x /= display_metrics_.device_pixel_ratio;
    (*it)->y /= display_metrics_.device_pixel_ratio;
  }

  if (sky_view_)
    sky_view_->HandlePointerPacket(packet);
}

void Engine::RunFromLibrary(const std::string& name) {
  TRACE_EVENT0("flutter", "Engine::RunFromLibrary");
  sky_view_ = blink::SkyView::Create(this);
  sky_view_->CreateView(name);
  sky_view_->RunFromLibrary(name, dart_library_provider_.get());
  sky_view_->SetDisplayMetrics(display_metrics_);
  sky_view_->SetLocale(language_code_, country_code_);
  if (!initial_route_.empty())
    sky_view_->PushRoute(initial_route_);
}

void Engine::RunFromSnapshotStream(
    const std::string& name,
    mojo::ScopedDataPipeConsumerHandle snapshot) {
  TRACE_EVENT0("flutter", "Engine::RunFromSnapshotStream");
  sky_view_ = blink::SkyView::Create(this);
  sky_view_->CreateView(name);
  sky_view_->RunFromSnapshot(name, snapshot.Pass());
  sky_view_->SetDisplayMetrics(display_metrics_);
  sky_view_->SetLocale(language_code_, country_code_);
  if (!initial_route_.empty())
    sky_view_->PushRoute(initial_route_);
}

void Engine::RunFromPrecompiledSnapshot(const mojo::String& bundle_path) {
  TRACE_EVENT0("flutter", "Engine::RunFromPrecompiledSnapshot");
  AssetUnpackerJob* unpacker = new AssetUnpackerJob(
      mojo::GetProxy(&root_bundle_), base::WorkerPool::GetTaskRunner(true));
  std::string path_str = bundle_path;
  unpacker->Unpack(Fetch(base::FilePath(path_str)));

  sky_view_ = blink::SkyView::Create(this);
  sky_view_->CreateView("http://localhost");
  sky_view_->RunFromPrecompiledSnapshot();
  sky_view_->SetDisplayMetrics(display_metrics_);
  sky_view_->SetLocale(language_code_, country_code_);
  if (!initial_route_.empty())
    sky_view_->PushRoute(initial_route_);
}

void Engine::RunFromFile(const mojo::String& main,
                         const mojo::String& package_root) {
  TRACE_EVENT0("flutter", "Engine::RunFromFile");
  std::string package_root_str = package_root;
  dart_library_provider_.reset(
      new DartLibraryProviderFiles(base::FilePath(package_root_str)));
  RunFromLibrary(main);
}

void Engine::RunFromBundle(const mojo::String& path) {
  TRACE_EVENT0("flutter", "Engine::RunFromBundle");
  std::string path_str = path;
  ZipAssetBundle::Create(mojo::GetProxy(&root_bundle_),
                         base::FilePath(path_str),
                         base::WorkerPool::GetTaskRunner(true));

  root_bundle_->GetAsStream(kSnapshotKey,
                            base::Bind(&Engine::RunFromSnapshotStream,
                                       weak_factory_.GetWeakPtr(), path_str));
}

void Engine::RunFromAssetBundle(const mojo::String& url,
                                mojo::asset_bundle::AssetBundlePtr bundle) {
  TRACE_EVENT0("flutter", "Engine::RunFromAssetBundle");
  std::string url_str = url;
  root_bundle_ = bundle.Pass();
  root_bundle_->GetAsStream(kSnapshotKey,
                            base::Bind(&Engine::RunFromSnapshotStream,
                                       weak_factory_.GetWeakPtr(), url_str));
}

void Engine::PushRoute(const mojo::String& route) {
  if (sky_view_)
    sky_view_->PushRoute(route);
  else
    initial_route_ = route;
}

void Engine::PopRoute() {
  if (sky_view_)
    sky_view_->PopRoute();
}

void Engine::OnAppLifecycleStateChanged(sky::AppLifecycleState state) {
  switch (state) {
    case sky::AppLifecycleState::PAUSED:
      activity_running_ = false;
      StopAnimator();
      break;

    case sky::AppLifecycleState::RESUMED:
      activity_running_ = true;
      StartAnimatorIfPossible();
      break;
  }

  if (sky_view_)
    sky_view_->OnAppLifecycleStateChanged(state);
}

void Engine::DidCreateIsolate(Dart_Isolate isolate) {
  Internals::Create(isolate, services_.Pass(), root_bundle_.Pass());
}

void Engine::StopAnimator() {
  animator_->Stop();
}

void Engine::StartAnimatorIfPossible() {
  if (activity_running_ && have_surface_)
    animator_->Start();
}

void Engine::ScheduleFrame() {
  animator_->RequestFrame();
}

void Engine::FlushRealTimeEvents() {
  animator_->FlushRealTimeEvents();
}

void Engine::Render(std::unique_ptr<flow::LayerTree> layer_tree) {
}

}  // namespace shell
}  // namespace sky
