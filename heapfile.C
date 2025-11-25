#include "heapfile.h"
#include "error.h"

/*
        Creates: empty (almost empty) heap file using db->createfile()

        Allocate empty page using bm->allocPage(), which returns a pointer to empty page in buffer pool, with page number.
        Use the pointer to init values for header page.

        Call bm->allocPage() to create Page pointer that is the first data page of the file.
        Call pointer's init() method to init page contents.

        Store page number of data page in firstPage and lastPage attributes of FileHdrPage.
        Unpin both pages and mark as dirty.
*/

// routine to create a heapfile
const Status createHeapFile(const string fileName)
{
    File*               file;
    Status              status;
    FileHdrPage*        hdrPage;
    int                 hdrPageNo;
    int                 newPageNo;
    Page*               newPage;

    // try to open the file. This should return an error
    status = db.openFile(fileName, file);
    if (status != OK)
    {
                // file doesn't exist. First create it and allocate
                // an empty header page and data page.

        status = db.createFile(fileName);
        if (status != OK) return status;

        status = db.openFile(fileName, file);
        if (status != OK) return status;

        // Allocate header page
        status = bufMgr->allocPage(file, hdrPageNo, newPage);
        if (status != OK) return status;

        hdrPage = (FileHdrPage*) newPage;

        // Initialize header page
        strcpy(hdrPage->fileName, fileName.c_str());
        hdrPage->pageCnt = 1;
        hdrPage->recCnt = 0;
        hdrPage->firstPage = -1;
        hdrPage->lastPage = -1;

        // Allocate first data page
        status = bufMgr->allocPage(file, newPageNo, newPage);
        if (status != OK) return status;

        newPage->init(newPageNo);
        hdrPage->firstPage = newPageNo;
        hdrPage->lastPage = newPageNo;
        hdrPage->pageCnt = 2;

        // Unpin and mark pages dirty
        status = bufMgr->unPinPage(file, newPageNo, true);
        if (status != OK) return status;
        status = bufMgr->unPinPage(file, hdrPageNo, true);
        if (status != OK) return status;

        status = db.closeFile(file);
        if (status != OK) return status;

        return OK;

    }
    return (FILEEXISTS);

}

// routine to destroy a heapfile
const Status destroyHeapFile(const string fileName)
{
        return (db.destroyFile (fileName));
}

/*
        1. Open file using db.openFile()
        2. Read and pin header page for file in bugger pool, init private data fields (headerPage, headerPageNo, hdrDirtyFlag)
        3. Read and pin first page of file into buffer pool, init curPage, curPageNO, curDirtyFlag
        4. Set curRec to NULLRID

        TIP: Use file->getFirstPage() to get page number of header page
*/
// constructor opens the underlying file
HeapFile::HeapFile(const string & fileName, Status& returnStatus)
{
    Status      status;
    Page*       pagePtr;

    // cout << "opening file " << fileName << endl;

    // open the file and read in the header page and the first data page
    if ((status = db.openFile(fileName, filePtr)) == OK)
    {
                // Read the header page
        int newPageNo;
        status = filePtr->getFirstPage(newPageNo);
        if (status != OK) { returnStatus = status; return; }

        status = bufMgr->readPage(filePtr, newPageNo, pagePtr);
        if (status != OK) { returnStatus = status; return; }

        headerPage = (FileHdrPage*) pagePtr;
        headerPageNo = newPageNo;
        hdrDirtyFlag = false;

        // Read the first data page
        curPageNo = headerPage->firstPage;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK) { returnStatus = status; return; }

        curDirtyFlag = false;
        curRec = NULLRID;

        returnStatus = OK;

    }
    else
    {
        cerr << "open of heap file failed\n";
                returnStatus = status;
                return;
    }
}


// the destructor closes the file
HeapFile::~HeapFile()
{
    Status status;
    // cout << "invoking heapfile destructor on file " << headerPage->fileName << endl;

    // see if there is a pinned data page. If so, unpin it
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
                curPage = NULL;
                curPageNo = 0;
                curDirtyFlag = false;
                if (status != OK) cerr << "error in unpin of date page\n";
    }

         // unpin the header page
    status = bufMgr->unPinPage(filePtr, headerPageNo, hdrDirtyFlag);
    if (status != OK) cerr << "error in unpin of header page\n";

        // TODO Still need these?
        // status = bufMgr->flushFile(filePtr);  // make sure all pages of the file are flushed to disk
        // if (status != OK) cerr << "error in flushFile call\n";
        // before close the file
        status = db.closeFile(filePtr);
    if (status != OK)
    {
                cerr << "error in closefile call\n";
                Error e;
                e.print (status);
    }
}

// Return number of records in heap file

const int HeapFile::getRecCnt() const
{
  return headerPage->recCnt;
}

/*
        Returns pointer to record given corresponding RID

        If record is on the pinned page, call curPage->getRecord(rid, rec) to get the record.
        Otherwise, unpin current pinned page. Use pageNo to read page into buffer pool.
*/
const Status HeapFile::getRecord(const RID & rid, Record & rec)
{
    Status status;

    // cout<< "getRecord. record (" << rid.pageNo << "." << rid.slotNo << ")" << endl;

    // If no current page pinned OR wrong page pinned -> switch pages
    if (curPage == NULL || curPageNo != rid.pageNo)
    {
        if (curPage != NULL)
        {
            status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
            if (status != OK) return status;
        }

        curPageNo = rid.pageNo;
        status = bufMgr->readPage(filePtr, curPageNo, curPage);
        if (status != OK) return status;

        curDirtyFlag = false;
    }

    status = curPage->getRecord(rid, rec);
    if (status == OK) curRec = rid;

    return status;
}

HeapFileScan::HeapFileScan(const string & name, Status & status) : HeapFile(name, status)
{
    filter = NULL;
}

const Status HeapFileScan::startScan(const int offset_,
                                     const int length_,
                                     const Datatype type_,
                                     const char* filter_,
                                     const Operator op_)
{
    if (!filter_) { // no filtering requested
        filter = NULL;
        return OK;
    }

    if ((offset_ < 0 || length_ < 1) ||
        (type_ != STRING && type_ != INTEGER && type_ != FLOAT) ||
        (type_ == INTEGER && length_ != sizeof(int)
         || type_ == FLOAT && length_ != sizeof(float)) ||
        (op_ != LT && op_ != LTE && op_ != EQ && op_ != GTE && op_ != GT && op_ != NE))
    {
        return BADSCANPARM;
    }

    offset = offset_;
    length = length_;
    type = type_;
    filter = filter_;
    op = op_;

    return OK;
}


const Status HeapFileScan::endScan()
{
    Status status;
    // generally must unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
        curPage = NULL;
        curPageNo = 0;
                curDirtyFlag = false;
        return status;
    }
    return OK;
}

HeapFileScan::~HeapFileScan()
{
    endScan();
}

const Status HeapFileScan::markScan()
{
    // make a snapshot of the state of the scan
    markedPageNo = curPageNo;
    markedRec = curRec;
    return OK;
}

const Status HeapFileScan::resetScan()
{
    Status status;
    if (markedPageNo != curPageNo)
    {
                if (curPage != NULL)
                {
                        status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
                        if (status != OK) return status;
                }
                // restore curPageNo and curRec values
                curPageNo = markedPageNo;
                curRec = markedRec;
                // then read the page
                status = bufMgr->readPage(filePtr, curPageNo, curPage);
                if (status != OK) return status;
                curDirtyFlag = false; // it will be clean
    }
    else curRec = markedRec;
    return OK;
}

/*
        Returns RID of next record that fits scan filters

        1. Scan the file one page at a time.
        2. For each page, use firstRecord() and nextRecord() to get the RIDs for all records on page
        3. Check if record satisfies the scan filter
                - Convert RID to a pointer to the record data
                - Call matchRec()
        4. If match, store RID in curRec and return it.

        Returns: OK if no errors, otherwise returns error code of first error that occurred.
*/
const Status HeapFileScan::scanNext(RID& outRid)
{

    Status      status = OK;
    RID         nextRid;
    RID         tmpRid;
    int         nextPageNo;
    Record      rec;

    // EOF
    if (curPage == NULL && curPageNo == -1)
        return FILEEOF;

        // If no page is currently pinned, start from the first data page.
    if (curPage == NULL) {
        nextPageNo = headerPage->firstPage;
        status = bufMgr->readPage(filePtr, nextPageNo, curPage);
        if (status != OK) return status;

        curPageNo = nextPageNo;
        curDirtyFlag = false;
        curRec = NULLRID; // indicate we haven't started scanning this page
    }

    // Loop over pages until we either find a matching record or run out of pages.
    while (true)
    {
        // Attempt to get the next record on the current page.
        if (curRec.pageNo == -1) {
            status = curPage->firstRecord(nextRid);
        } else {
            status = curPage->nextRecord(curRec, nextRid);
        }

        if (status == OK) {
            // We have a candidate record on the pinned curPage.
            status = curPage->getRecord(nextRid, rec);
            if (status != OK) return status;

            if (matchRec(rec)) {
                // Found matching record — keep page pinned and return RID.
                curRec = nextRid;
                outRid = nextRid;
                return OK;
            }

            // Not a match: advance curRec and continue scanning the same page.
            curRec = nextRid;
            continue;
        }

        // No more records on this page; move to next page in the linked list.
        if (status == ENDOFPAGE || status == NORECORDS) {
                        // TODO check if NORECORDS ia returned by any func being called. Remove this comment if not needed

            // Get the next page number while curPage is still pinned
            status = curPage->getNextPage(nextPageNo);
            if (status != OK) return status;

            // Unpin the current page now that we're done with it
            status = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
            if (status != OK) return status;

            // Reset curPage state
                        // TODO need to make any changes here or ok?

            curPage = NULL;
            curPageNo = -1; //thinking if should change to -1 but InsertFileScan constructor use 0 too
            curDirtyFlag = false;
            curRec = NULLRID; // are we supposed to change if the rid returned last time was from the prev pinned page, based in how we get firstrecord it seems yes NULLRID is needed

            // If no next page, scanning finished
            if (nextPageNo < 0) return FILEEOF;

            // Read/pin the next page
            status = bufMgr->readPage(filePtr, nextPageNo, curPage);
            if (status != OK) return status;

            curPageNo = nextPageNo;
            curDirtyFlag = false;
            curRec = NULLRID; // start at first record of new page
            // loop back to scan this new page
            continue;
        }

        // Any other error: propagate it
        return status;
    }

    // unreachable
    return OK;
}


// returns pointer to the current record.  page is left pinned
// and the scan logic is required to unpin the page

const Status HeapFileScan::getRecord(Record & rec)
{
    return curPage->getRecord(curRec, rec);
}

// delete record from file.
const Status HeapFileScan::deleteRecord()
{
    Status status;

    // delete the "current" record from the page
    status = curPage->deleteRecord(curRec);
    curDirtyFlag = true;

    // reduce count of number of records in the file
    headerPage->recCnt--;
    hdrDirtyFlag = true;
    return status;
}


// mark current page of scan dirty
const Status HeapFileScan::markDirty()
{
    curDirtyFlag = true;
    return OK;
}

const bool HeapFileScan::matchRec(const Record & rec) const
{
    // no filtering requested
    if (!filter) return true;

    // see if offset + length is beyond end of record
    // maybe this should be an error???
    if ((offset + length -1 ) >= rec.length)
        return false;

    float diff = 0;                       // < 0 if attr < fltr
    switch(type) {

    case INTEGER:
        int iattr, ifltr;                 // word-alignment problem possible
        memcpy(&iattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ifltr,
               filter,
               length);
        diff = iattr - ifltr;
        break;

    case FLOAT:
        float fattr, ffltr;               // word-alignment problem possible
        memcpy(&fattr,
               (char *)rec.data + offset,
               length);
        memcpy(&ffltr,
               filter,
               length);
        diff = fattr - ffltr;
        break;

    case STRING:
        diff = strncmp((char *)rec.data + offset,
                       filter,
                       length);
        break;
    }

    switch(op) {
    case LT:  if (diff < 0.0) return true; break;
    case LTE: if (diff <= 0.0) return true; break;
    case EQ:  if (diff == 0.0) return true; break;
    case GTE: if (diff >= 0.0) return true; break;
    case GT:  if (diff > 0.0) return true; break;
    case NE:  if (diff != 0.0) return true; break;
    }

    return false;
}

InsertFileScan::InsertFileScan(const string & name,
                               Status & status) : HeapFile(name, status)
{
  // Do nothing. Heapfile constructor will read the header page and the first
  // data page of the file into the buffer pool
}

InsertFileScan::~InsertFileScan()
{
    Status status;
    // unpin last page of the scan
    if (curPage != NULL)
    {
        status = bufMgr->unPinPage(filePtr, curPageNo, true);
        curPage = NULL;
        curPageNo = 0;
        if (status != OK) cerr << "error in unpin of data page\n";
    }
}

/*
        Inserts record into file, returning RID of the inserted record (outRid)

        If can't insert into current page, create new page (with header page content, etc), and link the pages together.
        Then insert record into new page
*/
// Insert a record into the file
const Status InsertFileScan::insertRecord(const Record & rec, RID& outRid)
{
    Page*       newPage;
    int         newPageNo;
    Status      status, unpinstatus;
    RID         rid;

    // check for very large records
    if ((unsigned int) rec.length > PAGESIZE-DPFIXED)
    {
        // will never fit on a page, so don't even bother looking
        return INVALIDRECLEN;
    }

    // If curPage is NULL, start with the last page
    if (curPage == NULL) {
        status = bufMgr->readPage(filePtr, headerPage->lastPage, curPage);
        if (status != OK) return status;
        curPageNo = headerPage->lastPage;
        curDirtyFlag = false;
    }

    // Try inserting into current page
    status = curPage->insertRecord(rec, rid);
    if (status == OK) {
        // success — update bookkeeping
        outRid = rid;
        headerPage->recCnt++;
        hdrDirtyFlag = true;
        curDirtyFlag = true;
        return OK;
    }

    // If cannot insert, need to create a new page
    if (status != NOSPACE) return status;

    // Allocate new page
    status = bufMgr->allocPage(filePtr, newPageNo, newPage);
    if (status != OK) return status;

    newPage->init(newPageNo);
    newPage->setNextPage(-1);
    curPage->setNextPage(newPageNo);
    curDirtyFlag = true;

    // update file header
    headerPage->lastPage = newPageNo;
    headerPage->pageCnt++;
    hdrDirtyFlag = true;

    // Unpin old page
    unpinstatus = bufMgr->unPinPage(filePtr, curPageNo, curDirtyFlag);
    if (unpinstatus != OK) return unpinstatus;

    // New page becomes current

        // TODO check this?
    // DO I have to readpage new page here????
    curPage = newPage;
    curPageNo = newPageNo;
    curDirtyFlag = true;

    // insert again — this time must succeed
    status = curPage->insertRecord(rec, rid);
    if (status != OK) return status;

    outRid = rid;
    headerPage->recCnt++;
    hdrDirtyFlag = true;
    curDirtyFlag = true;

    return OK;

}
