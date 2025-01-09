//
// Created by kaylor chen on 2024/6/22.
//

#include "yolo_threadpool.h"

#include "kaylordut/log/logger.h"
#ifdef RK3588
#include "platform/rockchip/rk3588.h"
#endif
#ifdef ONNX
#include "platform/onnxruntime/onnxruntime.h"
#endif
#ifdef TRT
#include "platform/tensorrt/tensorrt.h"
#endif

YoloThreadpool::YoloThreadpool(std::string &model_path,
                               std::vector<float> &conf_threshold,
                               int threads) {
  try {
    this->num_threads_ = threads;
    this->threads_mutex_ = std::make_unique<std::mutex[]>(threads);
    this->pool_ = std::make_unique<ThreadPool>(threads);
    auto model_extension = model_path.substr(model_path.find_last_of('.') + 1);
    KAYLORDUT_LOG_INFO("model_path is {},  model_extension is {}", model_path,
                       model_extension);
#ifdef RK3588
    if (model_extension == "rknn") {
      CreateAiInstance<Rk3588>(model_path, threads);
    }
#endif
#ifdef ONNX
    if (model_extension == "onnx") {
      CreateAiInstance<OnnxRuntime>(model_path, threads);
    }
#endif
#ifdef TRT
    if (model_extension == "trt" || model_extension == "engine") {
      CreateAiInstance<TensorRT>(model_path, threads);
    }
#endif
    for (int i = 0; i < instances_.size(); ++i) {
      tensors_data_.push_back(std::make_shared<ai_framework::TensorData>(
          instances_.at(i)->get_config()));
      auto it = instances_.at(i)->get_config().input_layer_shape.begin();
      auto model_height = it->second.at(2);
      yolo_preprocess_.push_back(
          std::make_shared<YoloPreProcess>(model_height, false));
      yolo_postprocess_.push_back(std::make_shared<YoloPostProcess>(
          instances_.at(i)->get_config(), conf_threshold, 0.6, 0.5));
      instances_.at(i)->BindInputAndOutput(*tensors_data_.at(i));
    }
  } catch (const std::bad_alloc &e) {
    KAYLORDUT_LOG_ERROR("Out of memory: {}", e.what());
    exit(EXIT_FAILURE);
  }
  instances_.at(0)->PrintLayerInfo();
  KAYLORDUT_LOG_INFO("Yolo thread pool initialization completed")
}

void YoloThreadpool::AddInferenceTask(
    const std::vector<cv::Mat> &original_image, const double time_stamp,
    const bool clone_original_image) {
  //  std::vector<cv::Mat> image;
  //  if (clone_original_image) {
  //    for (int i = 0; i < original_image.size(); ++i) {
  //      image.push_back(original_image.at(i).clone());
  //    }
  //  } else {
  //    image = original_image;
  //  }
  auto &image = original_image;
  // lamda表达式传入参数需要使用值，不能使用引用，这里使用cv的智能指针，image可以获取保留数据的指针，如果使用引用的话，指向的数据会发生变化
  this->pool_->enqueue(
      [&](const std::vector<cv::Mat> image, const double time_stamp) {
        auto id = this->get_thread_id();
        std::lock_guard<std::mutex> lock_threads(this->threads_mutex_[id]);
        this->yolo_preprocess_.at(id)->Run(
            image, this->tensors_data_.at(id)->get_input_tensor_ptr());
        KAYLORDUT_TIME_COST_DEBUG("DoInference()",
                                  this->instances_.at(id)->DoInference());
        this->yolo_postprocess_.at(id)->Run(
            this->tensors_data_.at(id)->get_output_tensor_ptr());
        std::lock_guard<std::mutex> lock(this->result_mutex_);
        if (!yolo_inference_result_queue_.empty()) {
          if (time_stamp <= yolo_inference_result_queue_.back()->time_stamp) {
            KAYLORDUT_LOG_WARN(
                "current time stamp is too old, drop the result");
            return;
          }
        }
        auto res = std::make_shared<YoloInferenceResult>();
        res->time_stamp = time_stamp;
        res->original_image = image;
        res->results = this->yolo_postprocess_.at(id)->get_result();
        yolo_inference_result_queue_.push(res);
      },
      image, time_stamp);
}

std::shared_ptr<YoloThreadpool::YoloInferenceResult>
YoloThreadpool::GetInferenceResult() {
  std::lock_guard<std::mutex> lock(this->result_mutex_);
  if (yolo_inference_result_queue_.empty()) {
    return nullptr;
  } else {
    auto res = this->yolo_inference_result_queue_.front();
    this->yolo_inference_result_queue_.pop();
    return std::move(res);
  }
}

int YoloThreadpool::get_thread_id() {
  std::lock_guard<std::mutex> lock(id_mutex_);
  auto id = id_;
  ++id_;
  if (id_ == num_threads_) {
    id_ = 0;
  }
  return id;
}