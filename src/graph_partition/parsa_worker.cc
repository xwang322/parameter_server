#include "graph_partition/parsa_worker.h"
#include "data/stream_reader.h"
#include "base/localizer.h"
namespace PS {
namespace GP {

void ParsaWorker::init() {
  conf_.mutable_input_graph()->set_ignore_feature_group(true);
  num_partitions_ = conf_.parsa().num_partitions();
  neighbor_set_.resize(num_partitions_);
  sync_nbset_ = KVVectorPtr<Key, uint64>(new KVVector<Key, uint64>());
  REGISTER_CUSTOMER(app_cf_.parameter_name(0), sync_nbset_);
}

void ParsaWorker::partitionU() {
  StreamReader<Empty> stream(searchFiles(conf_.input_graph()));
  ProducerConsumer<BlockData> reader(conf_.parsa().data_buff_size_in_mb());
  int blk_id = 0;
  reader.startProducer([this, &stream, &blk_id](BlockData* blk, size_t* size)->bool {
      // read a block
      MatrixPtrList<Empty> X;
      auto examples = ExampleListPtr(new ExampleList());
      bool ret = stream.readMatrices(conf_.parsa().block_size(), &X, examples.get());
      if (X.empty()) return false;

      // find the unique keys
      auto G = std::static_pointer_cast<SparseMatrix<Key,Empty>>(X.back());
      G->info().set_type(MatrixInfo::SPARSE_BINARY);
      G->value().clear();
      Localizer<Key, Empty> localizer;
      SArray<Key> key;
      localizer.countUniqIndex(G, &key);

      // pull the current partition from servers
      MessagePtr pull(new Message(kServerGroup));
      pull->task.set_key_channel(blk_id);
      pull->setKey(key);
      pull->addFilter(FilterConfig::KEY_CACHING);
      sync_nbset_->key(blk_id) = key;
      int time = sync_nbset_->pull(pull);

      // preprocess data and store the results
      blk->row_major  = std::static_pointer_cast<Graph>(localizer.remapIndex(key));
      blk->col_major  = std::static_pointer_cast<Graph>(blk->row_major->toColMajor());
      blk->examples   = examples;
      blk->pull_time  = time;
      blk->blk_id     = blk_id ++;
      *size           = 1;

      // blk->global_key = key;
      return ret;
    });

  // writer
  typedef std::pair<ExampleListPtr, SArray<int>> ResultPair;
  ProducerConsumer<ResultPair> writer_1;
  std::vector<RecordWriter> proto_writers_1;
  std::vector<DataConfig> tmp_files;
  proto_writers_1.resize(num_partitions_);
  for (int i = 0; i < num_partitions_; ++i) {
    tmp_files.push_back(ithFile(conf_.output_graph(), 0, "_part_"+to_string(i)+"_tmp"));
    auto file = File::openOrDie(tmp_files.back(), "w");
    proto_writers_1[i] = RecordWriter(file);
  }
  writer_1.setCapacity(conf_.parsa().data_buff_size_in_mb());
  writer_1.startConsumer([&proto_writers_1](const ResultPair& data) {
      const auto& examples = *data.first;
      const auto& partition = data.second;
      CHECK_EQ(examples.size(), partition.size());
      for (int i = 0; i < examples.size(); ++i) {
        CHECK(proto_writers_1[partition[i]].WriteProtocolMessage(examples[i]));
      }
    });

  // partition U
  BlockData blk;
  SArray<int> map_U;
  int i = 0;
  while (reader.pop(&blk)) {
    partitionU(blk, &map_U);
    writer_1.push(std::make_pair(blk.examples, map_U));
    LL << i ++;
  }
  writer_1.setFinished();
  writer_1.waitConsumer();

  // reader & write
  // uint64 cost = 0;
  // for (int k = 0; k < num_partitions_; ++k) {
  //   std::unordered_set<Key> key_set;
  //   auto file = File::openOrDie(tmp_files[k], "r");
  //   RecordReader proto_reader(file);
  //   Example ex;
  //   while (proto_reader.ReadProtocolMessage(&ex)) {
  //     for (int i = 0;  i < ex.slot_size(); ++i) {
  //       const auto& slot = ex.slot(i);
  //       if (slot.id() == 0) continue;
  //       for (int j = 0; j < slot.key_size(); ++j) {
  //         auto key = slot.key(j);
  //         if (partition_V_[hash(key)] != k) {
  //           if (key_set.count(key) == 0) {
  //             key_set.insert(key);
  //             ++ cost;
  //           }
  //         }
  //       }
  //     }
  //   }
  // }
  // LL << cost;


  // producerConsumer<ResultPair> writer_2;
  // typedef std::pair<GraphPtrList, ExampleListPtr> DataPair;
  // StreamReader<Empty> stream(searchFiles(conf_.input_graph()));
  // ProducerConsumer<DataPair> reader(conf_.data_buff_size_in_mb());
  // reader.startProducer([this, &stream](DataPair* data, size_t* size)->bool {
}

void ParsaWorker::partitionU(const BlockData& blk, SArray<int>* map_U) {
  sync_nbset_->waitOutMsg(kServerGroup, blk.pull_time);
  int id = blk.blk_id;
  auto key = sync_nbset_->key(id);
  initNeighborSet(key, sync_nbset_->value(id));

  int n = blk.row_major->rows();
  map_U->resize(n);
  assigned_U_.clear();
  assigned_U_.resize(n);
  initCost(blk.row_major, key);

  // partitioning
  for (int i = 0; i < n; ++i) {
    // TODO sync if necessary

    // assing U_i to partition k
    int k = i % num_partitions_;
    int Ui = cost_[k].minIdx();
    assigned_U_.set(Ui);
    (*map_U)[Ui] = k;

    // update
    updateCostAndNeighborSet(blk.row_major, blk.col_major, key, Ui, k);
  }

  // send results to servers
  sendUpdatedNeighborSet(id);
}

void ParsaWorker::initNeighborSet(
    const SArray<Key>& global_key, const SArray<uint64>& nbset) {
  CHECK_EQ(global_key.size(), nbset.size());
  int n = global_key.size();

  added_neighbor_set_.resize(0);
  for (int i = 0; i < num_partitions_; ++i) {
#ifdef EXACT_NBSET
    neighbor_set_[i].clear();
#else
    int k = conf_.parsa().bloomfilter_k();
    int m = n * k * 1.44 * conf_.parsa().bloomfilter_m_ratio();
    neighbor_set_[i].resize(m, k);
#endif
  }

  for (int i = 0; i < n; ++i) {
    uint64 s = nbset[i];
    if (s == 0) continue;
    for (int k = 0; k < num_partitions_; ++k) {
      if (s & (1 << k)) {
        neighbor_set_[k].insert(global_key[i]);
      }
    }
  }
}

void ParsaWorker::sendUpdatedNeighborSet(int blk_id) {
  auto& nbset = added_neighbor_set_;
  if (nbset.empty()) return;

  // pack the local updates
  std::sort(nbset.begin(), nbset.end(),
            [](const KP& a, const KP& b) { return a.first < b.first; });
  SArray<Key> key;
  SArray<uint64> value;
  Key pre = nbset[0].first;
  uint64 s = 0;
  for (int i = 0; i < nbset.size(); ++i) {
    Key cur = nbset[i].first;
    if (cur != pre) {
      value.pushBack(s);
      key.pushBack(pre);
      pre = cur;
      s = 0;
    }
    s |= 1 << nbset[i].second;
  }
  key.pushBack(nbset.back().first);
  value.pushBack(s);

  // send local updates
  MessagePtr push(new Message(kServerGroup));
  push->task.set_key_channel(blk_id);
  push->setKey(key);
  push->addValue(value);
  push->addFilter(FilterConfig::KEY_CACHING)->set_clear_cache_if_done(true);
  sync_nbset_->set(push)->set_op(Operator::OR);
  sync_nbset_->push(push);

}

// init the cost of assigning U_i to partition k
void ParsaWorker::initCost(const GraphPtr& row_major_blk, const SArray<Key>& global_key) {
  int n = row_major_blk->rows();
  size_t* row_os = row_major_blk->offset().begin();
  uint32* row_idx = row_major_blk->index().begin();
  std::vector<int> cost(n);
  cost_.resize(num_partitions_);
  for (int k = 0; k < num_partitions_; ++k) {
    const auto& assigned_V = neighbor_set_[k];
    for (int i = 0; i < n; ++ i) {
      for (size_t j = row_os[i]; j < row_os[i+1]; ++j) {
       // cost[i] += !assigned_V[global_key[row_idx[j]]];
        cost[i] += !assigned_V.count(global_key[row_idx[j]]);
      }
    }
    cost_[k].init(cost, conf_.parsa().cost_cache_limit());
  }
}

void ParsaWorker::updateCostAndNeighborSet(
    const GraphPtr& row_major_blk, const GraphPtr& col_major_blk,
    const SArray<Key>& global_key, int Ui, int partition) {
  for (int s = 0; s < num_partitions_; ++s) cost_[s].remove(Ui);

  size_t* row_os = row_major_blk->offset().begin();
  size_t* col_os = col_major_blk->offset().begin();
  uint32* row_idx = row_major_blk->index().begin();
  uint32* col_idx = col_major_blk->index().begin();
  auto& assigned_V = neighbor_set_[partition];
  auto& cost = cost_[partition];
  for (size_t i = row_os[Ui]; i < row_os[Ui+1]; ++i) {
    int Vj = row_idx[i];
    auto key = global_key[Vj];
    // if (assigned_V[key]) continue;
    if (assigned_V.count(key)) continue;
    assigned_V.insert(key);
    added_neighbor_set_.pushBack(std::make_pair(key, (P)partition));
    for (size_t s = col_os[Vj]; s < col_os[Vj+1]; ++s) {
      int Uk = col_idx[s];
      if (assigned_U_[Uk]) continue;
      cost.decrAndSort(Uk);
    }
  }
}

} // namespace GP
} // namespace PS
