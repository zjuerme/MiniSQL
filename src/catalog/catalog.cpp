#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

/* 返回序列化的大小 */
uint32_t CatalogMeta::GetSerializedSize() const {
  //sizeof(uint32_t)*3+(table_meta_pages_.size()+index_meta_pages_.size())*(sizeof(uint32_t)+sizeof(int32_t));
  return 12 + 8 * (table_meta_pages_.size() + index_meta_pages_.size());;
}

CatalogMeta::CatalogMeta() {}

/* CatalogManager */
CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager),
      lock_manager_(lock_manager),
      log_manager_(log_manager) {
  if (init) {
    // step1: 实例化一个新的CatalogMeta
    catalog_meta_ = CatalogMeta::NewInstance();
    // 刷新CatalogManager的几个nextid
    next_table_id_ = catalog_meta_->GetNextTableId();
    next_index_id_ = catalog_meta_->GetNextIndexId();
  } else {
    // step1: 反序列化CatalogMetadata
    Page *meta_data_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
    catalog_meta_ = CatalogMeta::DeserializeFrom(meta_data_page->GetData());
    // step2: 刷新CatalogManager的几个nextid
    next_table_id_ = catalog_meta_->GetNextTableId();
    next_index_id_ = catalog_meta_->GetNextIndexId();
    // step3: 更新CatalogManager的数据
    for (auto table_meta_page_it : catalog_meta_->table_meta_pages_) {
      ASSERT(LoadTable(table_meta_page_it.first, table_meta_page_it.second) == DB_SUCCESS, "LoadTable Failed!");
    }
    for (auto index_meta_page_it : catalog_meta_->index_meta_pages_) {
      ASSERT(LoadIndex(index_meta_page_it.first, index_meta_page_it.second) == DB_SUCCESS, "LoadIndex Failed");
    }
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);
  }
  FlushCatalogMetaPage();
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

/* CreateTable */
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Transaction *txn,
                                    TableInfo *&table_info) {
  // step1: 检查table是否已经存在
  if (table_names_.find(table_name) != table_names_.end())
    return DB_TABLE_ALREADY_EXIST;
  // step2: 新建TableInfo,TableMetaData,TableHeap
  table_info = TableInfo::Create();
  table_id_t table_id = next_table_id_++;
  Schema *deep_copy_schema = Schema::DeepCopySchema(schema);
  TableHeap *table_heap = TableHeap::Create(buffer_pool_manager_, deep_copy_schema, nullptr, log_manager_, lock_manager_);
  TableMetadata *meta_data = TableMetadata::Create(table_id, table_name, table_heap->GetFirstPageId(), deep_copy_schema);
  table_info->Init(meta_data, table_heap);
  // step3: 更新CatalogManager和CatalogMetaData
  table_names_[table_name] = table_id;
  tables_[table_id] = table_info;
  page_id_t meta_data_page_id;
  Page *meta_data_page = buffer_pool_manager_->NewPage(meta_data_page_id);
  ASSERT(meta_data_page != nullptr, "NULL in New a table meta data Page");
  meta_data->SerializeTo(meta_data_page->GetData());
  catalog_meta_->table_meta_pages_[table_id] = meta_data_page_id;
  buffer_pool_manager_->UnpinPage(meta_data_page_id,true);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/*查找table_name，然后将其对应的table_info存到给的参数里面*/
dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto table_id_it = table_names_.find(table_name);
  if (table_id_it == table_names_.end())
    return DB_TABLE_NOT_EXIST;
  return GetTable(table_id_it->second, table_info);
}

/* GetTables */
dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  for (auto table : tables_) {
    tables.push_back(table.second);
  }
  return DB_SUCCESS;
}

/* CreateIndex */
dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Transaction *txn,
                                    IndexInfo *&index_info, const string &index_type) {
  // step1: 检查Table是否已经存在，Index是否已经存在
  if (table_names_.find(table_name) == table_names_.end()) return DB_TABLE_NOT_EXIST;
  if (index_names_[table_name].find(index_name) != index_names_[table_name].end()) return DB_INDEX_ALREADY_EXIST;
  // step2: 新建IndexMetaData,IndexInfo
  index_info = IndexInfo::Create();
  index_id_t index_id = next_index_id_++;
  table_id_t table_id = table_names_.find(table_name)->second;
  TableInfo *table_info = tables_[table_id];
  std::vector<uint32_t> key_map;
  for (const auto &index_key_name : index_keys) {
    uint32_t key_index;
    if (table_info->GetSchema()->GetColumnIndex(index_key_name, key_index) == DB_COLUMN_NAME_NOT_EXIST)
      return DB_COLUMN_NAME_NOT_EXIST;
    key_map.push_back(key_index);
  }
  // 新建IndexMetaData并init index_info
  IndexMetadata *meta_data = IndexMetadata::Create(index_id, index_name, table_id, key_map);
  index_info->Init(meta_data, table_info, buffer_pool_manager_);
  // step3: 更新CatalogMetaData和CatalogManager
  if (index_names_.find(table_name) == index_names_.end()){
    std::unordered_map<std::string, index_id_t> map;
    map[index_name] = index_id;
    index_names_[table_name] = map;
  }
  else {// index_names_里面已有对应的table
    index_names_.find(table_name)->second[index_name] = index_id;
  }
  indexes_[index_id] = index_info;
  page_id_t meta_data_page_id;
  Page *meta_data_page = buffer_pool_manager_->NewPage(meta_data_page_id);
  ASSERT(meta_data_page != nullptr, "NULL in New a table meta data Page");
  meta_data->SerializeTo(meta_data_page->GetData());
  catalog_meta_->index_meta_pages_[index_id] = meta_data_page_id;
  buffer_pool_manager_->UnpinPage(meta_data_page_id, true);
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/* GetIndex */
dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  // 获取对应的table存放的Index情况
  auto index_name_it = table_names_.find(table_name);
  if (index_name_it == table_names_.end()) return DB_TABLE_NOT_EXIST;
  // 获取对应的Index存放的情况
  auto table_index_it = index_names_.find(table_name);
  if (table_index_it == index_names_.end()){ // index_names里没有这个table_name，说明这个table没有index
    return DB_INDEX_NOT_FOUND;
  }
  else{
    auto index_id_it = table_index_it->second.find(index_name);
    if (index_id_it == table_index_it->second.end()) return DB_INDEX_NOT_FOUND;
    // 从indexes_中拿到对应的Info并赋值
    auto index_info_it = indexes_.find(index_id_it->second);
    if (index_info_it == indexes_.end()) return DB_FAILED;
    index_info = index_info_it->second;
  }
  return DB_SUCCESS;
}

/* 根据table_name，将对应所有的index的IndexInfo放到参数里面 */
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
  // 获取对应的table存放的Index情况
  auto map = table_names_.find(table_name);
  if (map == table_names_.end())
      return DB_TABLE_NOT_EXIST;
  // 获取对应的Index存放的情况并Push到indexes中
  if (index_names_.find(table_name) == index_names_.end()){// 该table没有对应的index
  }
  else{
    for (const auto &index_map : index_names_.find(table_name)->second) {
      auto indexes_it = indexes_.find(index_map.second);
      if (indexes_it == indexes_.end()) return DB_FAILED;
      indexes.push_back(indexes_it->second);
    }
  }
  return DB_SUCCESS;
}

// 删除对应名字的table
dberr_t CatalogManager::DropTable(const string &table_name) {
  // 查找是否存在table
  auto table_id_it = table_names_.find(table_name);
  if (table_id_it == table_names_.end()) return DB_TABLE_NOT_EXIST;
  table_id_t table_id = table_id_it->second;
  // 删除储存table的页和储存matadata的页
  if (!buffer_pool_manager_->DeletePage(tables_[table_id]->GetRootPageId())) return DB_FAILED;
  if (!buffer_pool_manager_->DeletePage(catalog_meta_->table_meta_pages_[table_id])) return DB_FAILED;
  // 删除各个map中对应的table
  tables_.erase(tables_.find(table_id));
  table_names_.erase(table_names_.find(table_name));
  catalog_meta_->table_meta_pages_.erase(catalog_meta_->table_meta_pages_.find(table_id));
  // 回收各个map中该table的index
  if (index_names_.find(table_name) != index_names_.end()){
    for (const auto &index_pair : index_names_[table_name]){
      catalog_meta_->index_meta_pages_.erase(index_pair.second);
      indexes_.erase(index_pair.second);
    }
    index_names_.erase(table_name);
  }
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

/* 根据参数删除对应的index */
dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  // 同DropTable,目前并没有回收index_id以及info占用的内存的打算
  // 查找table是否存在
  if (table_names_.find(table_name) == table_names_.end()) return DB_TABLE_NOT_EXIST;
  auto table_index_it = index_names_.find(table_name);
  // 查找是否这个table没有index
  if (table_index_it == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  } else {
    // 查找是否这个table的Index没有所求的Index
    auto index_name_it = table_index_it->second.find(index_name);
    if (index_name_it == table_index_it->second.end())
      return DB_INDEX_NOT_FOUND;
    else {
      // 删除该索引以及存放metadata的数据页
      index_id_t index_id = index_name_it->second;
      IndexInfo *index_info = indexes_[index_id];
      // 删除索引
      index_info->GetIndex()->Destroy();
      // 删除存放metadata的页
      if (!buffer_pool_manager_->DeletePage(catalog_meta_->index_meta_pages_[index_id])) return DB_FAILED;
      // 从index_names_删除该index
      if (table_index_it->second.size() == 1) {
        // 如果这个table只有这个Index
        index_names_.erase(table_index_it);
      } else {
        table_index_it->second.erase(index_name);
      }
      // 从indexes_中删除该index
      indexes_.erase(index_id);
      // 在catalog_meta_data中删除
      catalog_meta_->index_meta_pages_.erase(catalog_meta_->index_meta_pages_.find(index_id));
    }
  }
  FlushCatalogMetaPage();
  return DB_SUCCESS;
}


/* FlushCatalogMetaPage()  */
dberr_t CatalogManager::FlushCatalogMetaPage() const {
  // 直接序列化CatalogMetaData到数据页中
  auto catalog_meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  ASSERT(catalog_meta_page != nullptr, "read catalog_meta_page failed!");
  //  catalog_meta_page->WLatch();
  catalog_meta_->SerializeTo(catalog_meta_page->GetData());
  //  catalog_meta_page->WUnlatch();
  // 立即将数据转存到磁盘
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  buffer_pool_manager_->FlushPage(CATALOG_META_PAGE_ID);

  return DB_SUCCESS;
}

/* 读取page_id存的table_meta_data,并更新CatalogManager*/
dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  // 拿到存meta_data的页
  auto meta_data_page = buffer_pool_manager_->FetchPage(page_id);
  ASSERT(meta_data_page != nullptr, "Fetch Tabel_meta_data_page failed!");
  // 新建TableMetaData并反序列化
  TableInfo *table_info = TableInfo::Create();
  // meta_data_page->RLatch();
  TableMetadata *meta_data = nullptr;
  TableMetadata::DeserializeFrom(meta_data_page->GetData(), meta_data);
  //  meta_data_page->RUnlatch();
  ASSERT(table_id == meta_data->GetTableId(), "False Table ID in LoadTable!");
  // 插入table_names_
  table_names_[meta_data->GetTableName()] = table_id;
  // init table_info插入tables_
  // 新建table_heap
  TableHeap *table_heap = TableHeap::Create(buffer_pool_manager_, meta_data->GetFirstPageId(), meta_data->GetSchema(),log_manager_, lock_manager_);
  table_info->Init(meta_data, table_heap);
  tables_[table_id] = table_info;
  buffer_pool_manager_->UnpinPage(page_id, false);
  return DB_SUCCESS;
}


/* 读取page_id存的table_meta_data,并更新CatalogManager */
dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  // 拿到存meta_data的页
  auto meta_data_page = buffer_pool_manager_->FetchPage(page_id);
  ASSERT(meta_data_page != nullptr, "Fetch Tabel_meta_data_page failed!");
  // 新建IndexMetaData并反序列化
  IndexInfo *index_info = IndexInfo::Create();
  IndexMetadata *meta_data = nullptr;
  IndexMetadata::DeserializeFrom(meta_data_page->GetData(), meta_data);
  ASSERT(index_id == meta_data->GetIndexId(), "False Index ID in LoadIndex!");
  // 插入index_names_
  table_id_t table_id = meta_data->GetTableId();
  std::string index_name = meta_data->GetIndexName();
  std::string table_name = tables_[table_id]->GetTableName();
  // 更新CatalogManage的map
  if (table_names_.find(table_name) == table_names_.end()) {
    // 没有对应的table
    buffer_pool_manager_->UnpinPage(page_id, false);
    return DB_TABLE_NOT_EXIST;
  } else {
    // 有对应的table
    if (index_names_.find(table_name) == index_names_.end()) {
      // index_names_里面没有该table
      std::unordered_map<std::string, index_id_t> map;
      map[index_name] = index_id;
      index_names_[table_name] = map;
    } else {
      // index_names_里面已有对应的table
      index_names_.find(table_name)->second[index_name] = index_id;
    }
  }
  // init index_info并插入indexes_
  index_info->Init(meta_data, tables_[table_id], buffer_pool_manager_);
  indexes_[index_id] = index_info;
  buffer_pool_manager_->UnpinPage(page_id, false);
  return DB_SUCCESS;
}

/* 将table_id的TableInfo填到参数table_info里 */
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto table_it = tables_.find(table_id);
  if (table_it == tables_.end())
    return DB_TABLE_NOT_EXIST;
  table_info = table_it->second;
  return DB_SUCCESS;
}