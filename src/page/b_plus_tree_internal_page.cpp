#include "page/b_plus_tree_internal_page.h"

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(page_id_t))
#define key_off 0
#define val_off GetKeySize()


/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
//void InternalPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
//}
void InternalPage::Init(page_id_t page_id, page_id_t parent_id , int key_size, int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetSize(0);
  SetMaxSize(max_size);
  SetKeySize(key_size);
}
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *InternalPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void InternalPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

page_id_t InternalPage::ValueAt(int index) const {
  return *reinterpret_cast<const page_id_t *>(pairs_off + index * pair_size + val_off);
}

void InternalPage::SetValueAt(int index, page_id_t value) {
  *reinterpret_cast<page_id_t *>(pairs_off + index * pair_size + val_off) = value;
}

int InternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == value)
      return i;
  }
  return -1;
}

void *InternalPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void InternalPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(page_id_t)));
}
/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 * 用了二分查找
 */
/* 自己填的 */
page_id_t InternalPage::Lookup(const GenericKey *key, const KeyManager &KM) {
//  return INVALID_PAGE_ID;
  int size = GetSize();
  page_id_t result;
  int i;
  for (i = 1; i < size; i++) {
    if (KM.CompareKeys(key, KeyAt(i)) < 0) {
      result = ValueAt(i-1);
      break;
    }
  }
  if (i == size)
    result = ValueAt(i-1);
  return result;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 */
/*填的*/
void InternalPage::PopulateNewRoot(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  SetValueAt(0 ,old_value);
  SetKeyAt(1, new_key);
  SetValueAt(1, new_value);
  SetSize(2);
}

/*
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
/*填的*/
int InternalPage::InsertNodeAfter(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  int size = GetSize();
  int old_location = ValueIndex(old_value);
  for (int i = size - 1; i > old_location; i--) {
    SetKeyAt(i+1, KeyAt(i));
    SetValueAt(i+1, ValueAt(i));
//    PairCopy(PairPtrAt(i+1), PairPtrAt(i), 1);
  }
  SetKeyAt(old_location + 1, new_key);
  SetValueAt(old_location + 1, new_value);
  IncreaseSize(1);
  return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 * buffer_pool_manager 是干嘛的？传给CopyNFrom()用于Fetch数据页
 */
/*填的*/
void InternalPage::MoveHalfTo(InternalPage *recipient, BufferPoolManager *buffer_pool_manager) {
  // assert(recipient != NULL);
  int size = GetSize();
  // assert(size == GetMaxSize() + 1);
  int start = GetMaxSize() / 2;
  int length = size - start;
  recipient->CopyNFrom(pairs_off  + GetMaxSize() / 2, length, buffer_pool_manager);
  SetSize(GetMinSize());
}

/* Copy entries into me, starting from {items} and copy {size} entries.
 * Since it is an internal page, for all entries (pages) moved, their parents page now changes to me.
 * So I need to 'adopt' them by changing their parent page id, which needs to be persisted with BufferPoolManger
 *
 */
/*填的*/
void InternalPage::CopyNFrom(void *src, int size, BufferPoolManager *buffer_pool_manager) {
  //  std::copy(&src, &src + size, pairs_off + GetSize());
  PairCopy(PairPtrAt(GetSize()), src, size);  //这句可能有问题？ TODO
  for (int i = GetSize(); i < GetSize() + size; i++) {
    Page *child_page = buffer_pool_manager->FetchPage(ValueAt(i));
    BPlusTreePage *child_node = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
    child_node->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(child_page->GetPageId(), true);
  }
  IncreaseSize(size);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array offset)
 * NOTE: store key&value pair continuously after deletion
 */
/*填的*/
void InternalPage::Remove(int index) {
  int size = GetSize();
  if (index >= 0 && index < size) {
    for (int i = index + 1; i < size; i++){
      SetKeyAt(i - 1, KeyAt(i));
      SetValueAt(i - 1, ValueAt(i));
    }
    IncreaseSize(-1);
  }
}

/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 */
/*填的*/
page_id_t InternalPage::RemoveAndReturnOnlyChild() {
  SetSize(0);
  return ValueAt(0);
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all key & value pairs from this page to "recipient" page.
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
/*填的*/
void InternalPage::MoveAllTo(InternalPage *recipient, GenericKey *middle_key, BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  // assert(GetSize() + recipient->GetSize() <= GetMaxSize());
  SetKeyAt(0, middle_key);
  recipient->CopyNFrom(pairs_off, size, buffer_pool_manager);
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to tail of "recipient" page.
 *
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
/*填的*/
void InternalPage::MoveFirstToEndOf(InternalPage *recipient, GenericKey *middle_key,
                                    BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  // assert(GetSize() + recipient->GetSize() <= GetMaxSize());
  SetKeyAt(0, middle_key);
  recipient->CopyNFrom(pairs_off, size, buffer_pool_manager);
  SetSize(0);
}

/* Append an entry at the end.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
/*填的*/
void InternalPage::CopyLastFrom(GenericKey *key, const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  //array_[size] = pair;
  PairCopy(PairPtrAt(size), key, 1); //这句可能有问题？ TODO
  Page *page = buffer_pool_manager->FetchPage(ValueAt(size));
  BPlusTreePage *datapage = reinterpret_cast<BPlusTreePage *>(page->GetData());
  datapage->SetParentPageId(GetPageId());
  buffer_pool_manager->UnpinPage(page->GetPageId(), true);
  IncreaseSize(1);

}

/*
 * Remove the last key & value pair from this page to head of "recipient" page.
 * You need to handle the original dummy key properly, e.g. updating recipient’s array to position the middle_key at the
 * right place.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those pages that are
 * moved to the recipient
 */
/*填的*/
void InternalPage::MoveLastToFrontOf(InternalPage *recipient, GenericKey *middle_key,
                                     BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  recipient->SetKeyAt(0, middle_key);
//  recipient->CopyFirstFrom(array_[size - 1], buffer_pool_manager);
  recipient->CopyLastFrom(KeyAt(size - 1), ValueAt(size - 1), buffer_pool_manager);
  IncreaseSize(-1);
}
/* Append an entry at the beginning.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
/*填的*/
void InternalPage::CopyFirstFrom(const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  for (int i = size; i > 0; i--) {
//    array_[i] = array_[i - 1];
    PairCopy(PairPtrAt(i), PairPtrAt(i - 1), 1);
  }
//  array_[0] = pair;
  PairCopy(PairPtrAt(0), PairPtrAt(0), 1);
  Page *page = buffer_pool_manager->FetchPage(ValueAt(0));
  BPlusTreePage *datapage = reinterpret_cast<BPlusTreePage *>(page->GetData());
  datapage->SetParentPageId(GetPageId());
  buffer_pool_manager->UnpinPage(page->GetPageId(), true);
  Page *parent = buffer_pool_manager->FetchPage(GetParentPageId());
  //B_PLUS_TREE_INTERNAL_PAGE_TYPE *p = reinterpret_cast<B_PLUS_TREE_INTERNAL_PAGE_TYPE *>(parent);
  BPlusTreeInternalPage *p = reinterpret_cast<BPlusTreeInternalPage *>(parent->GetData());
  int x = p->ValueIndex(GetPageId());
  p->SetKeyAt(x, KeyAt(0));
  buffer_pool_manager->UnpinPage(parent->GetPageId(), true);
  IncreaseSize(1);
}