#include <leveldb/db.h>
#include <stdint.h>

#include <fstream>  // NOLINT(readability/streams)
#include <string>
#include <vector>

#include "caffe/layer.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"
#include "caffe/vision_layers.hpp"

namespace caffe {

// This function is used to create a pthread that prefetches the data.
template <typename Dtype>
void DataLayer<Dtype>::InternalThreadEntry() {
  Datum datum;
  CHECK(prefetch_data_.count());
  Dtype* top_data = prefetch_data_.mutable_cpu_data();
  Dtype* top_label = NULL;  // suppress warnings about uninitialized variables
  if (output_labels_) {
    top_label = prefetch_label_.mutable_cpu_data();
  }
  const Dtype scale = this->layer_param_.data_param().scale();
  const int batch_size = this->layer_param_.data_param().batch_size();
  const int crop_size = this->layer_param_.data_param().crop_size();
  const bool mirror = this->layer_param_.data_param().mirror();

  if (mirror && crop_size == 0) {
    LOG(FATAL) << "Current implementation requires mirror and crop_size to be "
        << "set at the same time.";
  }
  // datum scales
  const int channels = datum_channels_;
  const int height = datum_height_;
  const int width = datum_width_;
  const int size = datum_size_;
  const Dtype* mean = data_mean_.cpu_data();
  for (int item_id = 0; item_id < batch_size; ++item_id) {
    // get a blob
    switch (this->layer_param_.data_param().backend()) {
    case DataParameter_DB_LEVELDB:
      CHECK(iter_);
      CHECK(iter_->Valid());
      datum.ParseFromString(iter_->value().ToString());
      break;
    case DataParameter_DB_LMDB:
      CHECK_EQ(mdb_cursor_get(mdb_cursor_, &mdb_key_,
              &mdb_value_, MDB_GET_CURRENT), MDB_SUCCESS);
      datum.ParseFromArray(mdb_value_.mv_data,
          mdb_value_.mv_size);
      break;
    default:
      LOG(FATAL) << "Unknown database backend";
    }

    const string& data = datum.data();
    if (crop_size) {
      CHECK(data.size()) << "Image cropping only support uint8 data";
      int h_off, w_off;
      // We only do random crop when we do training.
      if (phase_ == Caffe::TRAIN) {
        h_off = PrefetchRand() % (height - crop_size);
        w_off = PrefetchRand() % (width - crop_size);
      } else {
        h_off = (height - crop_size) / 2;
        w_off = (width - crop_size) / 2;
      }
      if (mirror && PrefetchRand() % 2) {
        // Copy mirrored version
        for (int c = 0; c < channels; ++c) {
          for (int h = 0; h < crop_size; ++h) {
            for (int w = 0; w < crop_size; ++w) {
              int top_index = ((item_id * channels + c) * crop_size + h)
                              * crop_size + (crop_size - 1 - w);
              int data_index = (c * height + h + h_off) * width + w + w_off;
              Dtype datum_element =
                  static_cast<Dtype>(static_cast<uint8_t>(data[data_index]));
              top_data[top_index] = (datum_element - mean[data_index]) * scale;
            }
          }
        }
      } else {
        // Normal copy
        for (int c = 0; c < channels; ++c) {
          for (int h = 0; h < crop_size; ++h) {
            for (int w = 0; w < crop_size; ++w) {
              int top_index = ((item_id * channels + c) * crop_size + h)
                              * crop_size + w;
              int data_index = (c * height + h + h_off) * width + w + w_off;
              Dtype datum_element =
                  static_cast<Dtype>(static_cast<uint8_t>(data[data_index]));
              top_data[top_index] = (datum_element - mean[data_index]) * scale;
            }
          }
        }
      }
    } else {
      // we will prefer to use data() first, and then try float_data()
      if (data.size()) {
        for (int j = 0; j < size; ++j) {
          Dtype datum_element =
              static_cast<Dtype>(static_cast<uint8_t>(data[j]));
          top_data[item_id * size + j] = (datum_element - mean[j]) * scale;
        }
      } else {
        for (int j = 0; j < size; ++j) {
          top_data[item_id * size + j] =
              (datum.float_data(j) - mean[j]) * scale;
        }
      }
    }

    if (output_labels_) {
      top_label[item_id] = datum.label();
    }
    // go to the next iter
    switch (this->layer_param_.data_param().backend()) {
    case DataParameter_DB_LEVELDB:
      iter_->Next();
      if (!iter_->Valid()) {
        // We have reached the end. Restart from the first.
        DLOG(INFO) << "Restarting data prefetching from start.";
        iter_->SeekToFirst();
      }
      break;
    case DataParameter_DB_LMDB:
      if (mdb_cursor_get(mdb_cursor_, &mdb_key_,
              &mdb_value_, MDB_NEXT) != MDB_SUCCESS) {
        // We have reached the end. Restart from the first.
        DLOG(INFO) << "Restarting data prefetching from start.";
        CHECK_EQ(mdb_cursor_get(mdb_cursor_, &mdb_key_,
                &mdb_value_, MDB_FIRST), MDB_SUCCESS);
      }
      break;
    default:
      LOG(FATAL) << "Unknown database backend";
    }
  }
}

template <typename Dtype>
DataLayer<Dtype>::~DataLayer<Dtype>() {
  JoinPrefetchThread();
  // clean up the database resources
  switch (this->layer_param_.data_param().backend()) {
  case DataParameter_DB_LEVELDB:
    break;  // do nothing
  case DataParameter_DB_LMDB:
    mdb_cursor_close(mdb_cursor_);
    mdb_close(mdb_env_, mdb_dbi_);
    mdb_txn_abort(mdb_txn_);
    mdb_env_close(mdb_env_);
    break;
  default:
    LOG(FATAL) << "Unknown database backend";
  }
}

template <typename Dtype>
void DataLayer<Dtype>::SetUp(const vector<Blob<Dtype>*>& bottom,
      vector<Blob<Dtype>*>* top) {
  Layer<Dtype>::SetUp(bottom, top);
  if (top->size() == 1) {
    output_labels_ = false;
    CHECK_EQ(this->layer_param_.data_param().label_dim(), 1) << "label_dim > 1 specified but labels are not even used";
  } else {
    output_labels_ = true;
    CHECK_GE(this->layer_param_.data_param().label_dim(), 1) << "label_dim should be 1 or greater";
  }
  
  // Init to suppress warnings about uninitialized variables
  //hdf_num_files_ = -1;
  //hdf_current_file_ = -1;
  //hdf_current_row_ = -1;

  // Initialize DB
  switch (this->layer_param_.data_param().backend()) {
  case DataParameter_DB_LEVELDB:
    {
    CHECK_EQ(this->layer_param_.data_param().label_dim(), 1) << "label_dim != 1 only supported for HDF5 for now";
    leveldb::DB* db_temp;
    leveldb::Options options;
    options.create_if_missing = false;
    options.max_open_files = 100;
    LOG(INFO) << "Opening leveldb " << this->layer_param_.data_param().source();
    leveldb::Status status = leveldb::DB::Open(
        options, this->layer_param_.data_param().source(), &db_temp);
    CHECK(status.ok()) << "Failed to open leveldb "
                       << this->layer_param_.data_param().source() << std::endl
                       << status.ToString();
    db_.reset(db_temp);
    iter_.reset(db_->NewIterator(leveldb::ReadOptions()));
    iter_->SeekToFirst();
    }
    break;
  case DataParameter_DB_LMDB:
    CHECK_EQ(this->layer_param_.data_param().label_dim(), 1) << "label_dim != 1 only supported for HDF5 for now";
    CHECK_EQ(mdb_env_create(&mdb_env_), MDB_SUCCESS) << "mdb_env_create failed";
    CHECK_EQ(mdb_env_set_mapsize(mdb_env_, 1099511627776), MDB_SUCCESS);  // 1TB
    CHECK_EQ(mdb_env_open(mdb_env_,
             this->layer_param_.data_param().source().c_str(),
             MDB_RDONLY|MDB_NOTLS, 0664), MDB_SUCCESS) << "mdb_env_open failed";
    CHECK_EQ(mdb_txn_begin(mdb_env_, NULL, MDB_RDONLY, &mdb_txn_), MDB_SUCCESS)
        << "mdb_txn_begin failed";
    CHECK_EQ(mdb_open(mdb_txn_, NULL, 0, &mdb_dbi_), MDB_SUCCESS)
        << "mdb_open failed";
    CHECK_EQ(mdb_cursor_open(mdb_txn_, mdb_dbi_, &mdb_cursor_), MDB_SUCCESS)
        << "mdb_cursor_open failed";
    LOG(INFO) << "Opening lmdb " << this->layer_param_.data_param().source();
    CHECK_EQ(mdb_cursor_get(mdb_cursor_, &mdb_key_, &mdb_value_, MDB_FIRST),
        MDB_SUCCESS) << "mdb_cursor_get failed";
    break;
  case DataParameter_DB_HDF5:
    {
    // Read the source to parse the filenames.
    const string& source = this->layer_param_.data_param().source();
    LOG(INFO) << "Loading HDF5 filenames from " << source;
    hdf_filenames_.clear();
    std::ifstream source_file(source.c_str());
    if (source_file.is_open()) {
      std::string line;
      while (source_file >> line) {
        hdf_filenames_.push_back(line);
      }
    }
    source_file.close();
    hdf_num_files_ = hdf_filenames_.size();
    hdf_current_file_ = 0;
    hdf_current_row_ = 0;
    LOG(INFO) << "Number of files: " << hdf_num_files_;

    // Load the first HDF5 file and initialize the line counter.
    // Before: 
    LoadNextHdfBatch();
    // After: these are updated and filled with data
    //   unsigned int hdf_current_file_;
    //   hsize_t hdf_current_row_;
    //   Blob<Dtype> buffer_data_;  // For partial reads and reads of uncropped images
    //   Blob<Dtype> buffer_label_; // For partial reads
    }
    break;
  default:
    LOG(FATAL) << "Unknown database backend";
  }

  // Check if we would need to randomly skip a few data points
  if (this->layer_param_.data_param().rand_skip()) {
    unsigned int skip = caffe_rng_rand() %
                        this->layer_param_.data_param().rand_skip();
    LOG(INFO) << "Skipping first " << skip << " data points.";
    while (skip-- > 0) {
      switch (this->layer_param_.data_param().backend()) {
      case DataParameter_DB_LEVELDB:
        iter_->Next();
        if (!iter_->Valid()) {
          iter_->SeekToFirst();
        }
        break;
      case DataParameter_DB_LMDB:
        if (mdb_cursor_get(mdb_cursor_, &mdb_key_, &mdb_value_, MDB_NEXT)
            != MDB_SUCCESS) {
          CHECK_EQ(mdb_cursor_get(mdb_cursor_, &mdb_key_, &mdb_value_,
                   MDB_FIRST), MDB_SUCCESS);
        }
        break;
      case DataParameter_DB_HDF5:
        LOG(FATAL) << "rand_skip parameter not yet supported for HDF5 backend";
        break;
      default:
        LOG(FATAL) << "Unknown database backend";
      }
    }
  }

  // Figure out the shape of each data point
  if (this->layer_param_.data_param().backend() == DataParameter_DB_HDF5) {
    datum_channels_ = buffer_data_.channels();
    datum_height_ = buffer_data_.height();
    datum_width_ = buffer_data_.width();
  } else {
    // Read a data point, and use it to initialize the top blob.
    Datum datum;
    switch (this->layer_param_.data_param().backend()) {
    case DataParameter_DB_LEVELDB:
      datum.ParseFromString(iter_->value().ToString());
      break;
    case DataParameter_DB_LMDB:
      datum.ParseFromArray(mdb_value_.mv_data, mdb_value_.mv_size);
      break;
    case DataParameter_DB_HDF5:
      // TODO: read first point into datum, somehow??
      break;
    default:
      LOG(FATAL) << "Unknown database backend";
    }
    datum_channels_ = datum.channels();
    datum_height_ = datum.height();
    datum_width_ = datum.width();
  }
  // datum size
  datum_size_ = datum_channels_ * datum_height_ * datum_width_;

  // image
  int crop_size = this->layer_param_.data_param().crop_size();
  CHECK_GT(datum_height_, crop_size);
  CHECK_GT(datum_width_, crop_size);
  if (crop_size > 0) {
    (*top)[0]->Reshape(this->layer_param_.data_param().batch_size(),
                       datum_channels_, crop_size, crop_size);
    prefetch_data_.Reshape(this->layer_param_.data_param().batch_size(),
        datum_channels_, crop_size, crop_size);
  } else {
    (*top)[0]->Reshape(
        this->layer_param_.data_param().batch_size(), datum_channels_,
        datum_height_, datum_width_);
    prefetch_data_.Reshape(this->layer_param_.data_param().batch_size(),
        datum_channels_, datum_height_, datum_width_);
  }
  LOG(INFO) << "output data size: " << (*top)[0]->num() << ","
      << (*top)[0]->channels() << "," << (*top)[0]->height() << ","
      << (*top)[0]->width();

  // label
  if (output_labels_) {
    (*top)[1]->Reshape(this->layer_param_.data_param().batch_size(), label_channels_, 1, 1);
    prefetch_label_.Reshape(this->layer_param_.data_param().batch_size(),
        label_channels_, 1, 1);
  }
  // check if we want to have mean
  if (this->layer_param_.data_param().has_mean_file()) {
    const string& mean_file = this->layer_param_.data_param().mean_file();
    LOG(INFO) << "Loading mean file from" << mean_file;
    BlobProto blob_proto;
    ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);
    data_mean_.FromProto(blob_proto);
    CHECK_EQ(data_mean_.num(), 1);
    CHECK_EQ(data_mean_.channels(), datum_channels_);
    CHECK_EQ(data_mean_.height(), datum_height_);
    CHECK_EQ(data_mean_.width(), datum_width_);
  } else {
    // Simply initialize an all-empty mean.
    data_mean_.Reshape(1, datum_channels_, datum_height_, datum_width_);
  }
  // Now, start the prefetch thread. Before calling prefetch, we make two
  // cpu_data calls so that the prefetch thread does not accidentally make
  // simultaneous cudaMalloc calls when the main thread is running. In some
  // GPUs this seems to cause failures if we do not so.
  prefetch_data_.mutable_cpu_data();
  if (output_labels_) {
    prefetch_label_.mutable_cpu_data();
  }
  data_mean_.cpu_data();
  DLOG(INFO) << "Initializing prefetch";
  CreatePrefetchThread();
  DLOG(INFO) << "Prefetch initialized.";
}


template <typename Dtype>
void DataLayer<Dtype>::LoadNextHdfBatch() {
  // not sure if all are needed...
  const unsigned batch_size = this->layer_param_.hdf5_data_param().batch_size();
  //const int data_count = (*top)[0]->count() / (*top)[0]->num();              // JBY: size of one data point = 3*256*256 = 196608
  //const int label_data_count = (*top)[1]->count() / (*top)[1]->num();        // JBY: size of one label = 1
  
  // Load the first HDF5 file and initialize the line counter.
  // Before: 
  // Inside -----> LoadNextHdfBatch();
  // After: these are updated and filled with data
  //   unsigned int hdf_current_file_;
  //   hsize_t hdf_current_row_;
  //   Blob<Dtype> buffer_data_;  // For partial reads and reads of uncropped images
  //   Blob<Dtype> buffer_label_; // For partial reads

  unsigned loaded_so_far = 0; // How much of buffer_data_ and buffer_label_ have been filled

  while (loaded_so_far < batch_size) {
    // Load next blob
    // Open next file
    
    // at start of loop:
    //  - hdf_current_file_ is valid
    //  - hdf_current_row_ is valid

    string& filename = hdf_filenames_[hdf_current_file_];
    LOG(INFO) << "Loading HDF5 file: " << filename;
    hid_t file_id = H5Fopen(filename.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    CHECK_GE(file_id, 0) << "Failed opening HDF5 file " << filename;
    
    const int MIN_DATA_DIM = 2;
    const int MAX_DATA_DIM = 4;
    const int MIN_LABEL_DIM = 1;
    const int MAX_LABEL_DIM = 2;

    float foo = 5.0;
    hdf5_load_nd_dataset(file_id, "data", MIN_DATA_DIM, foo,
                         &buffer_data_);
    hdf5_load_nd_dataset(file_id, "data", MIN_DATA_DIM, MAX_DATA_DIM,
                         &buffer_data_, 0, 0);
    hdf5_load_nd_dataset(file_id, "data", MIN_DATA_DIM, MAX_DATA_DIM,
                         &buffer_data_, hdf_current_row_, batch_size - loaded_so_far);
    hdf5_load_nd_dataset(file_id, "label", MIN_LABEL_DIM, MAX_LABEL_DIM,
                         &buffer_label_, hdf_current_row_, batch_size - loaded_so_far);

    herr_t status = H5Fclose(file_id);
    CHECK_GE(status, 0) << "Failed to close HDF5 file " << filename;
    CHECK_EQ(buffer_data_.num(), buffer_label_.num()) << "read a different number of data points vs. labels";

    int loaded_here = buffer_data_.num();
    LOG(INFO) << "Loaded " << loaded_here << " examples from " << filename;
    if (loaded_here == batch_size) {
      // We loaded everything we need, and there may well be more in the file
      hdf_current_row_ += loaded_here;
    } else {
      // We couldn't load enough, so the file must be exhausted. Go to next file.
      hdf_current_file_ += 1;
      if (hdf_current_file_ == hdf_num_files_) {
        hdf_current_file_ = 0;
        LOG(INFO) << "looping around to first HDF5 file";
      }
      hdf_current_row_ = 0;
    }

    // Copy the (possibly small) buffer to the (possibly larger) prefetch blob
    if (loaded_here > 0) {
      caffe_copy(buffer_data_.count(),
                 buffer_data_.cpu_data(),
                 prefetch_data_.mutable_cpu_data() + prefetch_data_.offset(loaded_so_far, 0, 0, 0));
      if (output_labels_) {
        caffe_copy(buffer_label_.count(),
                   buffer_label_.cpu_data(),
                   prefetch_label_.mutable_cpu_data() + prefetch_label_.offset(loaded_so_far, 0, 0, 0));
      }
      // Then at end:
      loaded_so_far += loaded_here;
    }
  }
}


template <typename Dtype>
void DataLayer<Dtype>::CreatePrefetchThread() {
  phase_ = Caffe::phase();
  const bool prefetch_needs_rand = (phase_ == Caffe::TRAIN) &&
      (this->layer_param_.data_param().mirror() ||
       this->layer_param_.data_param().crop_size());
  if (prefetch_needs_rand) {
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
  } else {
    prefetch_rng_.reset();
  }
  CHECK(!StartInternalThread()) << "Pthread execution failed";
}

template <typename Dtype>
void DataLayer<Dtype>::JoinPrefetchThread() {
  CHECK(!WaitForInternalThreadToExit()) << "Pthread joining failed";
}

template <typename Dtype>
unsigned int DataLayer<Dtype>::PrefetchRand() {
  CHECK(prefetch_rng_);
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  return (*prefetch_rng)();
}

template <typename Dtype>
Dtype DataLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
      vector<Blob<Dtype>*>* top) {
  // First, join the thread
  JoinPrefetchThread();
  // Copy the data
  caffe_copy(prefetch_data_.count(), prefetch_data_.cpu_data(),
             (*top)[0]->mutable_cpu_data());
  if (output_labels_) {
    caffe_copy(prefetch_label_.count(), prefetch_label_.cpu_data(),
               (*top)[1]->mutable_cpu_data());
  }
  // Start a new prefetch thread
  CreatePrefetchThread();
  return Dtype(0.);
}

#ifdef CPU_ONLY
STUB_GPU_FORWARD(DataLayer, Forward);
#endif

INSTANTIATE_CLASS(DataLayer);

}  // namespace caffe
