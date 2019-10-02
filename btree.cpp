/**
 * @author See Contributors.txt for code contributors and overview of BadgerDB.
 *
 * @section LICENSE
 * Copyright (c) 2012 Database Group, Computer Sciences Department, University of Wisconsin-Madison.
 */

#include "btree.h"
#include "filescan.h"
#include "exceptions/bad_index_info_exception.h"
#include "exceptions/bad_opcodes_exception.h"
#include "exceptions/bad_scanrange_exception.h"
#include "exceptions/no_such_key_found_exception.h"
#include "exceptions/scan_not_initialized_exception.h"
#include "exceptions/index_scan_completed_exception.h"
#include "exceptions/file_not_found_exception.h"
#include "exceptions/end_of_file_exception.h"


//#define DEBUG

namespace badgerdb
{

// -----------------------------------------------------------------------------
// BTreeIndex::BTreeIndex -- Constructor
// -----------------------------------------------------------------------------

BTreeIndex::BTreeIndex(const std::string & relationName,
		std::string & outIndexName,
		BufMgr *bufMgrIn,
		const int attrByteOffset,
		const Datatype attrType)
{
  leafOccupancy = INTARRAYLEAFSIZE;
  nodeOccupancy = INTARRAYNONLEAFSIZE;
  scanExecuting = false;
  bufMgr = bufMgrIn;

  std::ostringstream index_string;
  index_string << relationName << "." << attrByteOffset;
  outIndexName = index_string.str();

  try
  {
    file = new BlobFile(outIndexName, false);
    header_Num = file->getFirstPageNo();
    Page *header_Page;
    bufMgr->readPage(file, header_Num, header_Page);
    IndexMetaInfo *m = (IndexMetaInfo *)header_Page;
    root_Num = m->rootPageNo;

    if (relationName != m->relationName || attrType != m->attrType
      || attrByteOffset != m->attrByteOffset)
    {
      throw BadIndexInfoException(outIndexName);
    }

    bufMgr->unPinPage(file, header_Num, false);
  }

  catch(FileNotFoundException e)
  {

    file = new BlobFile(outIndexName, true);

    Page *header_Page;
    Page *rootPage;
    bufMgr->allocPage(file, header_Num, header_Page);
    bufMgr->allocPage(file, root_Num, rootPage);

    IndexMetaInfo *m = (IndexMetaInfo *)header_Page;
    m->attrByteOffset = attrByteOffset;
    m->attrType = attrType;
    m->rootPageNo = root_Num;
    strncpy((char *)(&(m->relationName)), relationName.c_str(), 20);
    m->relationName[19] = 0;
    initialroot = root_Num;

    LeafNodeInt *root = (LeafNodeInt *)rootPage;
    root->rightSibPageNo = 0;

    bufMgr->unPinPage(file, header_Num, true);
    bufMgr->unPinPage(file, root_Num, true);

    FileScan fileScan(relationName, bufMgr);
    RecordId rid;
    try
    {
      while(1)
      {
        fileScan.scanNext(rid);
        std::string record = fileScan.getRecord();
        insertEntry(record.c_str() + attrByteOffset, rid);
      }
    }
    catch(EndOfFileException e)
    {
      bufMgr->flushFile(file);
    }
  }
}


// -----------------------------------------------------------------------------
// BTreeIndex::~BTreeIndex -- destructor
// -----------------------------------------------------------------------------
BTreeIndex::~BTreeIndex(){
  scanExecuting = false;
  bufMgr->flushFile(BTreeIndex::file);
  delete file;
  file = nullptr;
}

// -----------------------------------------------------------------------------
// BTreeIndex::~BTreeIndex -- insertEntry
// -----------------------------------------------------------------------------
const void BTreeIndex::insertEntry(const void *key, const RecordId rid){
  RIDKeyPair<int> data;
  data.set(rid, *((int *)key));
  Page* root;
  bufMgr->readPage(file, root_Num, root);
  PageKeyPair<int> *newChild = nullptr;
  insert(root, root_Num, initialroot == root_Num ? true : false, data, newChild);
}

// -----------------------------------------------------------------------------
// BTreeIndex::~BTreeIndex -- nextNonleaf
// -----------------------------------------------------------------------------
const void BTreeIndex::nextNonleaf(NonLeafNodeInt *currentNode, PageId &nextNode, int check){
  int i = nodeOccupancy;
  while(i >= 0 && (currentNode->pageNoArray[i] == 0)){
    i--;
  }
  while(i > 0 && (currentNode->keyArray[i-1] >= check)){
    i--;
  }
  nextNode = currentNode->pageNoArray[i];
}

// -----------------------------------------------------------------------------
// BTreeIndex::update
// -----------------------------------------------------------------------------
const void BTreeIndex::update(PageId firstPageInRoot, PageKeyPair<int> *newChild){
  PageId newroot_Num;
  Page *newRoot;
  bufMgr->allocPage(file, newroot_Num, newRoot);
  NonLeafNodeInt *newRootPage = (NonLeafNodeInt *)newRoot;

  newRootPage->pageNoArray[0] = firstPageInRoot;
  newRootPage->pageNoArray[1] = newChild->pageNo;
  newRootPage->level = initialroot == root_Num ? 1 : 0;
  newRootPage->keyArray[0] = newChild->key;

  Page *m;
  bufMgr->readPage(file, header_Num, m);
  IndexMetaInfo *metaPage = (IndexMetaInfo *)m;
  metaPage->rootPageNo = newroot_Num;
  root_Num = newroot_Num;

  bufMgr->unPinPage(file, header_Num, true);
  bufMgr->unPinPage(file, newroot_Num, true);
}


// -----------------------------------------------------------------------------
// BTreeIndex::~BTreeIndex -- insert
// -----------------------------------------------------------------------------
const void BTreeIndex::insert(Page *currentPage, PageId currentPage_Num, bool node_leaf, const RIDKeyPair<int> data, PageKeyPair<int> *&newChild){
  if (!node_leaf)
  {
    NonLeafNodeInt *currentNode = (NonLeafNodeInt *)currentPage;
    Page *nextPage;
    PageId nextNode;
    nextNonleaf(currentNode, nextNode, data.key);
    bufMgr->readPage(file, nextNode, nextPage);
    node_leaf = currentNode->level == 1;
    insert(nextPage, nextNode, node_leaf, data, newChild);

    if (newChild == nullptr){
	    bufMgr->unPinPage(file, currentPage_Num, false);
    }
    else{
      if (currentNode->pageNoArray[nodeOccupancy] == 0) {
        nonleafInsertion(currentNode, newChild);
        newChild = nullptr;
        bufMgr->unPinPage(file, currentPage_Num, true);
      }
      else {
        nonleafSplit(currentNode, currentPage_Num, newChild);
      }
    }
  }
  else {
    LeafNodeInt *leaf = (LeafNodeInt *)currentPage;
    if (leaf->ridArray[leafOccupancy - 1].page_number == 0) {
      leafInsertion(leaf, data);
      bufMgr->unPinPage(file, currentPage_Num, true);
      newChild = nullptr;
    }
    else
    {
      leafSplit(leaf, currentPage_Num, newChild, data);
    }
  }
}

// -----------------------------------------------------------------------------
// BTreeIndex::leafSplit
// -----------------------------------------------------------------------------
const void BTreeIndex::leafSplit(LeafNodeInt *leaf, PageId leafPageNum, PageKeyPair<int> *&newChild, const RIDKeyPair<int> data){
  PageId newPageNum;
  Page *newPage;
  bufMgr->allocPage(file, newPageNum, newPage);
  LeafNodeInt *new_leafNode = (LeafNodeInt *)newPage;

  int median = leafOccupancy/2;

  if (leafOccupancy %2 == 1 && data.key > leaf->keyArray[median])
  {
    median = median + 1;
  }

  for(int i = median; i < leafOccupancy; i++)
  {
    new_leafNode->keyArray[i-median] = leaf->keyArray[i];
    new_leafNode->ridArray[i-median] = leaf->ridArray[i];
    leaf->keyArray[i] = 0;
    leaf->ridArray[i].page_number = 0;
  }

  if (data.key > leaf->keyArray[median-1])
  {
    leafInsertion(new_leafNode, data);
  }
  else
  {
    leafInsertion(leaf, data);
  }

  new_leafNode->rightSibPageNo = leaf->rightSibPageNo;
  leaf->rightSibPageNo = newPageNum;

  newChild = new PageKeyPair<int>();
  PageKeyPair<int> newPair;
  newPair.set(newPageNum, new_leafNode->keyArray[0]);
  newChild = &newPair;
  bufMgr->unPinPage(file, leafPageNum, true);
  bufMgr->unPinPage(file, newPageNum, true);

  if (leafPageNum == root_Num)
  {
    update(leafPageNum, newChild);
  }
}

const void BTreeIndex::leafInsertion(LeafNodeInt *leaf, RIDKeyPair<int> entry)
{

  if (leaf->ridArray[0].page_number == 0)  {
    leaf->ridArray[0] = entry.rid;
    leaf->keyArray[0] = entry.key;
  }
  else {
    int i = leafOccupancy - 1;
    while(i >= 0 && (leaf->ridArray[i].page_number == 0)) {
      i--;
    }
    while(i >= 0 && (leaf->keyArray[i] > entry.key)) {
      leaf->ridArray[i+1] = leaf->ridArray[i];
      leaf->keyArray[i+1] = leaf->keyArray[i];
      i--;
    }
    leaf->ridArray[i+1] = entry.rid;
    leaf->keyArray[i+1] = entry.key;
  }
}


// -----------------------------------------------------------------------------
// BTreeIndex::nonleafSplit
// -----------------------------------------------------------------------------
const void BTreeIndex::nonleafSplit(NonLeafNodeInt *p_node, PageId p_pageNum, PageKeyPair<int> *&newChild){
  PageId newPageNum;
  Page *newPage;
  bufMgr->allocPage(file, newPageNum, newPage);
  NonLeafNodeInt *newNode = (NonLeafNodeInt *)newPage;

  int median = nodeOccupancy/2;
  int p_index = median;
  PageKeyPair<int> p_entry;
  if (nodeOccupancy % 2 == 0) {
    p_index = newChild->key < p_node->keyArray[median] ? median -1 : median;
  }
  p_entry.set(newPageNum, p_node->keyArray[p_index]);

  median = p_index + 1;
  for(int i = median; i < nodeOccupancy; i++){
    newNode->keyArray[i-median] = p_node->keyArray[i];
    newNode->pageNoArray[i-median] = p_node->pageNoArray[i+1];
    p_node->pageNoArray[i+1] = (PageId) 0;
    p_node->keyArray[i+1] = 0;
  }

  newNode->level = p_node->level;
  p_node->keyArray[p_index] = 0;
  p_node->pageNoArray[p_index] = (PageId) 0;
  nonleafInsertion(newChild->key < newNode->keyArray[0] ? p_node : newNode, newChild);
  newChild = &p_entry;
  bufMgr->unPinPage(file, p_pageNum, true);
  bufMgr->unPinPage(file, newPageNum, true);

  if (p_pageNum == root_Num)  {
    update(p_pageNum, newChild);
  }
}


// -----------------------------------------------------------------------------
// BTreeIndex::nonleafInsertion
// -----------------------------------------------------------------------------
const void BTreeIndex::nonleafInsertion(NonLeafNodeInt *nonleaf, PageKeyPair<int> *entry){

  int i = nodeOccupancy;
  while(i >= 0 && (nonleaf->pageNoArray[i] == 0)){
    i--;
  }

  while( i > 0 && (nonleaf->keyArray[i-1] > entry->key)) {
    nonleaf->keyArray[i] = nonleaf->keyArray[i-1];
    nonleaf->pageNoArray[i+1] = nonleaf->pageNoArray[i];
    i--;
  }
  nonleaf->keyArray[i] = entry->key;
  nonleaf->pageNoArray[i+1] = entry->pageNo;
}

// -----------------------------------------------------------------------------
// BTreeIndex::startScan
// -----------------------------------------------------------------------------
const void BTreeIndex::startScan(const void* lowValue, const Operator lowPar, const void* highValue, const Operator highPar){
  low_initial = *((int *)lowValue);
  high_initial = *((int *)highValue);

  if(low_initial > high_initial){
    throw BadScanrangeException();
  }

  if(!((lowPar == GT or lowPar == GTE) and (highPar == LT or highPar == LTE))){
    throw BadOpcodesException();
  }

  lowOp = lowPar;
  highOp = highPar;

  if(scanExecuting){
    endScan();
  }

  currentPageNum = root_Num;
  bufMgr->readPage(file, currentPageNum, currentPageData);

  if(initialroot != root_Num){
    NonLeafNodeInt* currentNode = (NonLeafNodeInt *) currentPageData;
    bool foundLeaf = false;
    while(!foundLeaf) {
      currentNode = (NonLeafNodeInt *) currentPageData;
      if(currentNode->level == 1){
        foundLeaf = true;
      }

      PageId nextPageNum;
      nextNonleaf(currentNode, nextPageNum, low_initial);
      bufMgr->unPinPage(file, currentPageNum, false);
      currentPageNum = nextPageNum;
      bufMgr->readPage(file, currentPageNum, currentPageData);
    }
  }
  bool found = false;
  while(!found){
    LeafNodeInt* currentNode = (LeafNodeInt *) currentPageData;
    if(currentNode->ridArray[0].page_number == 0)
    {
      bufMgr->unPinPage(file, currentPageNum, false);
      throw NoSuchKeyFoundException();
    }
    bool nullVal = false;
    for(int i = 0; i < leafOccupancy and !nullVal; i++) {
      int key = currentNode->keyArray[i];
      if(i < leafOccupancy - 1 and currentNode->ridArray[i + 1].page_number == 0)
      {
        nullVal = true;
      }

      if(checkKey(low_initial, lowOp, high_initial, highOp, key))
      {
        next_entry = i;
        found = true;
        scanExecuting = true;
        break;
      }
      else if((highOp == LT and key >= high_initial) or (highOp == LTE and key > high_initial))
      {
        bufMgr->unPinPage(file, currentPageNum, false);
        throw NoSuchKeyFoundException();
      }

      if(i == leafOccupancy - 1 or nullVal){
        bufMgr->unPinPage(file, currentPageNum, false);
        if(currentNode->rightSibPageNo == 0)  {
          throw NoSuchKeyFoundException();
        }
        currentPageNum = currentNode->rightSibPageNo;
        bufMgr->readPage(file, currentPageNum, currentPageData);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// BTreeIndex::scanNext
// -----------------------------------------------------------------------------
const void BTreeIndex::scanNext(RecordId& outRid){
  if(!scanExecuting)
  {
    throw ScanNotInitializedException();
  }
	LeafNodeInt* currentNode = (LeafNodeInt *) currentPageData;
  if(currentNode->ridArray[next_entry].page_number == 0 or next_entry == leafOccupancy) {
    bufMgr->unPinPage(file, currentPageNum, false);
    if(currentNode->rightSibPageNo == 0) {
      throw IndexScanCompletedException();
    }
    currentPageNum = currentNode->rightSibPageNo;
    bufMgr->readPage(file, currentPageNum, currentPageData);
    currentNode = (LeafNodeInt *) currentPageData;
    next_entry = 0;
  }

  int key = currentNode->keyArray[next_entry];
  if(checkKey(low_initial, lowOp, high_initial, highOp, key)){
    outRid = currentNode->ridArray[next_entry];
    next_entry++;
  }
  else{
    throw IndexScanCompletedException();
  }
}

// -----------------------------------------------------------------------------
// BTreeIndex::checkKey
// -----------------------------------------------------------------------------
const bool BTreeIndex::checkKey(int lowValue, const Operator lowOp, int highValue, const Operator highOp, int check){
  if(lowOp == GTE && highOp == LTE){
    return check <= highValue && check >= lowValue;
  }
  else if(lowOp == GT && highOp == LTE){
    return check <= highValue && check > lowValue;
  }
  else if(lowOp == GTE && highOp == LT){
    return check < highValue && check >= lowValue;
  }
  else{
    return check < highValue && check > lowValue;
  }
}

// -----------------------------------------------------------------------------
// BTreeIndex::endScan
// -----------------------------------------------------------------------------
const void BTreeIndex::endScan(){
  if(!scanExecuting){
    throw ScanNotInitializedException();
  }
  scanExecuting = false;
  bufMgr->unPinPage(file, currentPageNum, false);
  currentPageNum = static_cast<PageId>(-1);
  currentPageData = nullptr;
  next_entry = -1;
}

}
