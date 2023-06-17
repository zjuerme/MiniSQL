#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

/**
 * TODO: Student Implement
 */
BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  if(leaf_max_size_==UNDEFINED_SIZE){
    leaf_max_size_=(int)((PAGE_SIZE-LEAF_PAGE_HEADER_SIZE)/(KM.GetKeySize()+sizeof(RowId))-1);
  }
  if(internal_max_size_==UNDEFINED_SIZE){
    internal_max_size_=(int)((PAGE_SIZE-INTERNAL_PAGE_HEADER_SIZE)/(KM.GetKeySize()+sizeof(page_id_t))-1);
  }
  IndexRootsPage *rootPage = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());
  bool ret = rootPage->GetRootId(index_id_, &root_page_id_);
  if (ret == false) {
    root_page_id_ = INVALID_PAGE_ID;
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  Page* current_page = buffer_pool_manager_->FetchPage(current_page_id);
  BPlusTreePage* current_node = reinterpret_cast<BPlusTreePage*>(current_page->GetData());
  if (current_node->IsLeafPage())
  {
    buffer_pool_manager_->UnpinPage(current_page_id, true);
    buffer_pool_manager_->DeletePage(current_page_id);
    return;
  }
  // recursive
  for (int i=0;i<current_node->GetSize();i++)
  {
    InternalPage* inter = reinterpret_cast<InternalPage *>(current_node);
    page_id_t child_pid = inter->ValueAt(i);
    Destroy(child_pid);
  }
  buffer_pool_manager_->UnpinPage(current_page_id, true);
  buffer_pool_manager_->DeletePage(current_page_id);
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Transaction *transaction) {
  if(IsEmpty())
    return false;
  LeafPage *leaf = reinterpret_cast<LeafPage *>(FindLeafPage(key,root_page_id_)->GetData());
  if (leaf == nullptr) 
  { // not found
    return false;
  }
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  // append to result vector
  if (found)
    result.push_back(value);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Transaction *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  page_id_t newPageId;
  Page *newPage = buffer_pool_manager_->NewPage(newPageId);
  if (newPage == nullptr)
  {
    throw std::bad_alloc();
  }
  LeafPage *newLeaf = reinterpret_cast<LeafPage *>(newPage->GetData());
  newLeaf->Init(newPageId, INVALID_PAGE_ID, processor_.GetKeySize(),leaf_max_size_);
  newLeaf->Insert(key,value,processor_);
  newLeaf->SetNextPageId(INVALID_PAGE_ID);
  buffer_pool_manager_->UnpinPage(newPageId, true);

  root_page_id_ = newPageId;  // Update the root page id
  UpdateRootPageId(true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Transaction *transaction) {
  LeafPage *leaf = reinterpret_cast<LeafPage *>(FindLeafPage(key,root_page_id_,false)->GetData());
  assert(leaf != nullptr); // for debug

  // check duplicate
  RowId value_discard;
  bool found = leaf->Lookup(key, value_discard, processor_);
  if (found) {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }
  if (leaf->Insert(key,value,processor_)<=leaf->GetMaxSize())
  {
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true); // dirty now
  }else{
    // split leaf page
    LeafPage *new_leaf = Split(leaf,transaction);
    InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf,transaction);

    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
  }
  return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Transaction *transaction) {
  page_id_t newPageId;
  Page *newPage= buffer_pool_manager_->NewPage(newPageId);
  if (newPage == nullptr)
  {
    throw std::bad_alloc();
  }
  BPlusTreeInternalPage * new_node = reinterpret_cast<BPlusTreeInternalPage  *>(newPage->GetData());
  new_node->Init(newPageId,node->GetParentPageId(),node->GetKeySize(),internal_max_size_);
  node->MoveHalfTo(new_node,buffer_pool_manager_);
  buffer_pool_manager_->UnpinPage(newPageId, true);
  return new_node;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Transaction *transaction) {
  page_id_t newPageId;
  cout<<"hello";
  Page *newPage= buffer_pool_manager_->NewPage(newPageId);
  if (newPage == nullptr)
  {
    throw std::bad_alloc();
  }
  BPlusTreeLeafPage * new_node = reinterpret_cast<BPlusTreeLeafPage *>(newPage->GetData());
  new_node->Init(newPageId,node->GetParentPageId(),processor_.GetKeySize(),leaf_max_size_);
  new_node->SetNextPageId(node->GetNextPageId());
  node->SetNextPageId(newPageId);
  node->MoveHalfTo(new_node);
  buffer_pool_manager_->UnpinPage(newPageId, true);
  return new_node;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node,
                                 Transaction *transaction) {
  if (old_node->IsRootPage())
  {
    page_id_t newRootPageId;
    Page *newPage = buffer_pool_manager_->NewPage(newRootPageId);
    if (newPage == nullptr)
    {
      throw std::bad_alloc();
    }
    InternalPage *new_root = reinterpret_cast<InternalPage *>(newPage->GetData());
    new_root->Init(newRootPageId, INVALID_PAGE_ID,processor_.GetKeySize(),internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
    old_node->SetParentPageId(newRootPageId);
    new_node->SetParentPageId(newRootPageId);
    root_page_id_ = newRootPageId;
    UpdateRootPageId();
    buffer_pool_manager_->UnpinPage(newRootPageId, true);
    buffer_pool_manager_->UnpinPage(old_node->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(new_node->GetPageId(), true);
  }else{
    BPlusTreeInternalPage *parent = reinterpret_cast<BPlusTreeInternalPage *>(buffer_pool_manager_->FetchPage(old_node->GetParentPageId())->GetData());
    if (parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId()) <= parent->GetMaxSize()) 
    {
      new_node->SetParentPageId(parent->GetPageId());
      buffer_pool_manager_->UnpinPage(old_node->GetPageId(), false);
      buffer_pool_manager_->UnpinPage(new_node->GetPageId(), true);
    }else{
      // parent split
      BPlusTreeInternalPage *parent_new_sibling = Split(parent, transaction);
      // update the parent of old_node and new_node
      // if (parent->ValueIndex(old_node->GetPageId())!=-1)
      //   old_node->SetParentPageId(parent->GetPageId());
      // else old_node->SetParentPageId(parent_new_sibling->GetPageId());
      // if (parent->ValueIndex(new_node->GetPageId())!=-1)
      //   new_node->SetParentPageId(parent->GetPageId());
      // else new_node->SetParentPageId(parent_new_sibling->GetPageId());
      InsertIntoParent(parent, parent_new_sibling->KeyAt(0), parent_new_sibling, transaction);
      buffer_pool_manager_->UnpinPage(old_node->GetPageId(), true);
      buffer_pool_manager_->UnpinPage(new_node->GetPageId(), true);
      buffer_pool_manager_->UnpinPage(parent_new_sibling->GetPageId(), true);
    }
    buffer_pool_manager_->UnpinPage(parent->GetPageId(), true);
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Transaction *transaction) {
  if (IsEmpty()) return;
  LeafPage *leaf = reinterpret_cast<LeafPage *>(FindLeafPage(key,root_page_id_,false)->GetData());
  if (leaf == nullptr) return;
  int index = leaf->KeyIndex(key,processor_);
  int size_after_delete = leaf->RemoveAndDeleteRecord(key, processor_);
  if (!leaf->IsRootPage())
  {
    auto parent_page=buffer_pool_manager_->FetchPage(leaf->GetParentPageId());
    while(index==0){
      InternalPage *parent=reinterpret_cast<InternalPage *>(parent_page->GetData());
      index=parent->ValueIndex(leaf->GetPageId());
      if (index==-1) break;
      if (index!=0) 
      {
        parent->SetKeyAt(index,leaf->KeyAt(0));
        buffer_pool_manager_->UnpinPage(parent->GetPageId(),true);
      }else 
      {
        if (!parent->IsRootPage())
        {
          parent_page=buffer_pool_manager_->FetchPage(parent->GetParentPageId());
          parent=reinterpret_cast<InternalPage *>(parent_page->GetData());
        }else break;

      }
    }
  }
  bool if_delete=false;
  if (size_after_delete < leaf->GetMinSize()) {
    if_delete = CoalesceOrRedistribute(leaf, transaction);
  }
  //buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Transaction *transaction) {
  assert(node->GetSize() < node->GetMinSize());
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }
  Page *node_page = buffer_pool_manager_->FetchPage(node->GetPageId());
  Page *parent_page = buffer_pool_manager_->FetchPage(node->GetParentPageId());
  InternalPage *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  int node_index_in_parent = parent->ValueIndex(node->GetPageId());
  int sibling_index;
  if (node_index_in_parent == 0) {
    sibling_index = node_index_in_parent + 1;
  } else {
    sibling_index = node_index_in_parent - 1;
  }
  Page *sibling_page = buffer_pool_manager_->FetchPage(parent->ValueAt(sibling_index));
  N *sibling = reinterpret_cast<N *>(sibling_page->GetData());
  bool parent_delete = false;
  bool leaf_delete = false;
  if (node->GetSize() + sibling->GetSize() > node->GetMaxSize()) {
    Redistribute(sibling, node, node_index_in_parent);
    buffer_pool_manager_->UnpinPage(node_page->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);
    buffer_pool_manager_->UnpinPage(sibling_page->GetPageId(), true);
  } else {
    parent_delete = Coalesce(sibling, node, parent, node_index_in_parent);
    //if (!parent_delete) buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);
    leaf_delete = true;
  }
  return leaf_delete;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Transaction *transaction) {
  assert(node->GetSize() + neighbor_node->GetSize() <= node->GetMaxSize());
  page_id_t nodeId = node->GetPageId();
  page_id_t neighborId = neighbor_node->GetPageId();

  if (index != 0) { // left sibling
    node->MoveAllTo(neighbor_node);
    // remove node from parent
    buffer_pool_manager_->UnpinPage(nodeId, true);
    buffer_pool_manager_->DeletePage(nodeId);
    buffer_pool_manager_->UnpinPage(neighborId, true);
    parent->Remove(index);
  } else {
    neighbor_node->MoveAllTo(node);
    // remove neighbor from parent
    buffer_pool_manager_->UnpinPage(nodeId, true);
    buffer_pool_manager_->UnpinPage(neighborId, true);
    buffer_pool_manager_->DeletePage(neighborId);
    parent->Remove(index + 1);
  }

  if (parent->GetSize() < parent->GetMinSize()) {
    // recursively if not enough size for parent
    return CoalesceOrRedistribute(parent, transaction);
  }
  return false;
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Transaction *transaction) {
  assert(node->GetSize() + neighbor_node->GetSize() <= node->GetMaxSize());
  page_id_t nodeId = node->GetPageId();
  page_id_t neighborId = neighbor_node->GetPageId();

  if (index != 0) { // left sibling
    node->MoveAllTo(neighbor_node,parent->KeyAt(index), buffer_pool_manager_);
    // remove node from parent
    buffer_pool_manager_->UnpinPage(nodeId, true);
    buffer_pool_manager_->DeletePage(nodeId);
    buffer_pool_manager_->UnpinPage(neighborId, true);
    parent->Remove(index);
  } else {
    neighbor_node->MoveAllTo(node,parent->KeyAt(index+1), buffer_pool_manager_);
    // remove neighbor from parent
    buffer_pool_manager_->UnpinPage(nodeId, true);
    buffer_pool_manager_->UnpinPage(neighborId, true);
    buffer_pool_manager_->DeletePage(neighborId);
    parent->Remove(index + 1);
  }

  if (parent->GetSize() < parent->GetMinSize()) {
    // recursively if not enough size for parent
    return CoalesceOrRedistribute(parent, transaction);
  }
  return false;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  page_id_t parentId = node->GetParentPageId();
  Page *parentPage = buffer_pool_manager_->FetchPage(parentId);
  InternalPage *parent = reinterpret_cast<InternalPage *>(parentPage->GetData());
  if (index == 0) { // right sibling
    neighbor_node->MoveFirstToEndOf(node);
    // update parent
    parent->SetKeyAt(index + 1, neighbor_node->KeyAt(0));
  } else { // left sibling
    neighbor_node->MoveLastToFrontOf(node);
    // update parent
    parent->SetKeyAt(index, node->KeyAt(0));
  }
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  page_id_t parentId = node->GetParentPageId();
  Page *parentPage = buffer_pool_manager_->FetchPage(parentId);
  InternalPage *parent = reinterpret_cast<InternalPage *>(parentPage->GetData());
  if (index == 0) { // right sibling
    auto page_index=parent->ValueIndex(neighbor_node->GetPageId());
    auto key=parent->KeyAt(page_index);
    neighbor_node->MoveFirstToEndOf(node, key,buffer_pool_manager_);
    // update parent
    parent->SetKeyAt(page_index, neighbor_node->KeyAt(0));
  } else { // left sibling
    auto page_index=parent->ValueIndex(node->GetPageId());
    auto key=parent->KeyAt(page_index);
    neighbor_node->MoveLastToFrontOf(node,key,buffer_pool_manager_);
    // update parent
    parent->SetKeyAt(page_index, node->KeyAt(0));
  }
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  if (old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId();
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), true);
    buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
    return true;
  }else if (old_root_node->GetSize() == 1){
    InternalPage *old_root = static_cast<InternalPage *>(old_root_node);
    root_page_id_ = old_root->RemoveAndReturnOnlyChild();
    UpdateRootPageId();
    Page *new_root_page = buffer_pool_manager_->FetchPage(root_page_id_);
    BPlusTreePage *new_root = reinterpret_cast<BPlusTreePage *>(new_root_page->GetData()); 
    new_root->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(root_page_id_, true);
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), true);
    buffer_pool_manager_->DeletePage(old_root_node->GetPageId());
    return true;
  }
  return false;

}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  GenericKey *key;
  Page *page=FindLeafPage(key,INVALID_PAGE_ID,true);
  auto leaf_page=reinterpret_cast<LeafPage *>(page->GetData());
  page_id_t leaf_id=leaf_page->GetPageId();
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),false);
  if(leaf_page->GetSize()==0)
    return IndexIterator(INVALID_PAGE_ID, buffer_pool_manager_);
  else
    return IndexIterator(leaf_id, buffer_pool_manager_);
}

/*
 * Input parameter is low-key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  auto leafpage=FindLeafPage(key,INVALID_PAGE_ID,false);
  LeafPage *leaf_page = reinterpret_cast<LeafPage *>(leafpage->GetData());
  page_id_t leaf_id=leaf_page->GetPageId();
  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(),false);

  RowId value;
  if(leaf_page->Lookup(key,value,processor_))
    return IndexIterator(leaf_id,buffer_pool_manager_,leaf_page->KeyIndex(key,processor_));
  else
    return IndexIterator(INVALID_PAGE_ID,buffer_pool_manager_);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  Page *page = FindLeafPage(nullptr,INVALID_PAGE_ID,true);
  LeafPage *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(),false);

  while(leaf->GetNextPageId()!=INVALID_PAGE_ID)
  {
    page_id_t nextpage=leaf->GetNextPageId();
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(),false);
    Page *next_page=buffer_pool_manager_->FetchPage(nextpage);
    leaf=reinterpret_cast<LeafPage *>(next_page->GetData());
  }
  return IndexIterator(leaf->GetPageId(),buffer_pool_manager_,leaf->GetSize());
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (IsEmpty()) {
    return nullptr;
  }

  Page *page;
  if(page_id==INVALID_PAGE_ID){
    page=buffer_pool_manager_->FetchPage(root_page_id_);
  }
  else{
    page=buffer_pool_manager_->FetchPage(page_id) ;
  }
  BPlusTreePage *node = reinterpret_cast<BPlusTreePage*>(page->GetData());

  while (!node->IsLeafPage()) {
    InternalPage *internal_node = reinterpret_cast<InternalPage *>(node);
    page_id_t next_page_id = leftMost ? internal_node->ValueAt(0) : internal_node->Lookup(key,processor_);
    Page* next_page = buffer_pool_manager_->FetchPage(next_page_id);
    BPlusTreePage * next_node = reinterpret_cast<BPlusTreePage*>(next_page->GetData());
    buffer_pool_manager_->UnpinPage(node->GetPageId(), false);
    page = next_page;
    node = next_node;
  }

  return page;
}

/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
  IndexRootsPage *root_page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());
  if (insert_record) {
    root_page->Insert(index_id_, root_page_id_);
  } else {
    root_page->Update(index_id_, root_page_id_);
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      out << "<TD>" << leaf->KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        out << inner->KeyAt(i);
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}