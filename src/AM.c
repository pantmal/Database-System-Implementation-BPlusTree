#include "AM.h"

#include "bf.h"
#include "defn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_OPEN_FILES 20
#define MAXSCANS 20

#define BLOCK_METADATA (sizeof(char) + 2*sizeof(int))


#define CALL_BF(call)       \
{                           \
  BF_ErrorCode code = call; \
  if (code != BF_OK) {         \
    BF_PrintError(code);    \
    AM_errno = BF_BLOCK_LEVEL_ERROR; \
    return AM_errno;        \
  }                         \
}

int AM_errno = AME_OK;


//OpenAM struct is used to keep track of important information regarding each file
typedef struct {
  char attrType1; 
  int attrLength1;
  char attrType2;
  int attrLength2;
  int root;
  int fd;
  int dataSize;
  int indexSize;
} openAM;

//Scan struct is used to keep track of important information regarding each open scan
typedef struct {
  int openAM;
  int blockNumber;
  int positionInBlock;
  int operation;
  void* key;
  void* value;
} scan;

openAM* tableOfIndexes[MAX_OPEN_FILES]; 
scan*  tableOfScans[MAXSCANS];

int openFiles = 0;
int openScans = 0;


// Calculate how many elements fit in a data(leaf) block.
int howManyInDataBlock(openAM* currentAM) {
  int keyPlusDataSize = currentAM->attrLength1 + currentAM->attrLength2;
  int count = (BF_BLOCK_SIZE - BLOCK_METADATA) / keyPlusDataSize;
  return count;
}


// Calculate how many elements fit in an index block.
int howManyInIndexBlock(openAM* currentAM) {
  int keyPlusPointerSize = currentAM->attrLength1 + sizeof(int);
  int count = (BF_BLOCK_SIZE - BLOCK_METADATA) / keyPlusPointerSize;
  return count;
}

//Compares value1 and keyValue arguments. 
//Returns a negative number if value1 is less than the key, 0 on equality or a positive integer otherwise.
int keyCompare(openAM* bplus, void* value1, void* keyValue) {
  if(bplus->attrType1 == 'i') {
    int value1Int;
    memcpy(&value1Int, value1, sizeof(int));
    return value1Int - *((int*)keyValue);
  }
  else if(bplus->attrType1 == 'f') {
    float value1Float;
    memcpy(&value1Float, value1, sizeof(float));
    return value1Float > *((float*)keyValue) ? 1 : (
        value1Float < *((float*)keyValue) ? -1 : 0);
  }
  else if(bplus->attrType1 == 'c'){
    return strncmp((char*)value1, (char*)keyValue, bplus->attrLength1);
  }
  else {
  	printf("Invalid type %c \n",bplus->attrLength1);
	  AM_errno = AME_INVALID_TYPE;
	  return AM_errno;   
  }
}

//RecursiveInsert adds the new item and returns 0 if no split occurred or the block number of the new created block.
int recursiveInsert(openAM* bplus, int currentBlockNum, void *value1, void *value2, void* returnKey) {
  //Get current block.
  BF_Block *currentBlock;
  BF_Block_Init(&currentBlock);
  CALL_BF(BF_GetBlock(bplus->fd, currentBlockNum, currentBlock));
  char* data = BF_Block_GetData(currentBlock);
  char* dataPoint = data;
  int sizeOfIndexElements = bplus->attrLength1 + sizeof(int);
  int sizeOfDataElements = bplus->attrLength1 + bplus->attrLength2;
  int count = 0;

  //If index block:
  if(data[0] == 'I') {
    dataPoint += sizeof(char);
    memcpy(&count, dataPoint, sizeof(int));
    dataPoint += sizeof(int);
    int visited = -1;
    int nextBlockNum;
    int insertResult;
    //Go to each element and if keyCompare returns negative, go to left pointer.
    for(int i = 0; i < count; i++) {
      if(keyCompare(bplus, value1, (void*)(dataPoint+sizeof(int))) < 0) {
        memcpy(&nextBlockNum, dataPoint, sizeof(int)); //Left pointer.
        insertResult = recursiveInsert(bplus, nextBlockNum, value1, value2, returnKey);
        visited = i;
        break;
      }
      dataPoint += sizeOfIndexElements;
    }
    //If no pointer visited, go to last pointer. 
    if(visited == -1) {
      memcpy(&nextBlockNum, dataPoint, sizeof(int)); //Last pointer.
      insertResult = recursiveInsert(bplus, nextBlockNum, value1, value2, returnKey);
    }

    //Split happened.
    if(insertResult != 0) {
      //Index split necessary. The middle key goes only to the parent with returnKey.
      if(bplus->indexSize == count) {
        char* buffer = (char*)malloc((bplus->indexSize + 1)*sizeOfIndexElements + sizeof(int));
        char* bufferPoint = buffer;
        //Copy all elements to buffer and add new one at the end.
        if(visited == -1) {
          memcpy(bufferPoint, data + sizeof(char) + sizeof(int), sizeOfIndexElements*count + sizeof(int));
          bufferPoint += sizeOfIndexElements*count + sizeof(int);
          memcpy(bufferPoint, returnKey, bplus->attrLength1);
          bufferPoint += bplus->attrLength1;
          memcpy(bufferPoint, &insertResult, sizeof(int));
        }
        else { //In this case the new element has to be placed in between the buffer.
          memcpy(bufferPoint, data + sizeof(char) + sizeof(int), sizeof(int));
          bufferPoint += sizeof(int);
          memcpy(bufferPoint, data + sizeof(char) + 2*sizeof(int), sizeOfIndexElements*visited);
          bufferPoint += sizeOfIndexElements*visited;
          memcpy(bufferPoint, returnKey, bplus->attrLength1);
          bufferPoint += bplus->attrLength1;
          memcpy(bufferPoint, &insertResult, sizeof(int));
          bufferPoint += sizeof(int);
          char* restData = data;
          restData += sizeof(char) + 2*sizeof(int) + sizeOfIndexElements*visited;
          memcpy(bufferPoint, restData, sizeOfIndexElements*(count - visited));
        }

        int halfPoint = (count + 1) / 2;

        //Copy first half of buffer to existing block.
        memcpy(data+sizeof(char)+2*sizeof(int), buffer+sizeof(int), halfPoint*sizeOfIndexElements);
        //Change original count.
        memcpy(data+sizeof(char), &halfPoint, sizeof(int));

        //Copy middle key one level up the recursion.
        bufferPoint = buffer;
        bufferPoint += sizeof(int)+halfPoint*sizeOfIndexElements;
        memcpy(returnKey, bufferPoint, bplus->attrLength1);
        bufferPoint += bplus->attrLength1;

        int numOfBlocks;
        CALL_BF(BF_GetBlockCounter(bplus->fd, &numOfBlocks));

        //Creating a new index block
        BF_Block *newBlock;
        BF_Block_Init(&newBlock);
        CALL_BF(BF_AllocateBlock(bplus->fd, newBlock));
        char* newData = BF_Block_GetData(newBlock);

        newData[0] = 'I';
        memcpy(newData+sizeof(char)+sizeof(int), bufferPoint, (count - halfPoint)*sizeOfIndexElements + sizeof(int));
        int newCount = count - halfPoint;
        memcpy(newData+sizeof(char), &newCount, sizeof(int));

        BF_Block_SetDirty(currentBlock);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);

        BF_Block_SetDirty(newBlock);
        CALL_BF(BF_UnpinBlock(newBlock));
        BF_Block_Destroy(&newBlock);

        free(buffer);
        return numOfBlocks;
      }
      else {//No split necessary, place the new item at the already existing block.
        if(visited == - 1) { //Placing the item at the end. 
          dataPoint += sizeof(int);
          memcpy(dataPoint, returnKey, bplus->attrLength1);
          dataPoint += bplus->attrLength1;
          memcpy(dataPoint, &insertResult, sizeof(int));
        }
        else { //Placing the item in between the already existing elements. 
          dataPoint += sizeof(int);
          memmove(dataPoint+sizeOfIndexElements, dataPoint, (count - visited)*sizeOfIndexElements);
          memcpy(dataPoint, returnKey, bplus->attrLength1);
          dataPoint += bplus->attrLength1;
          memcpy(dataPoint, &insertResult, sizeof(int));
        }

        int newCount = count + 1;
        memcpy(data+sizeof(char), &newCount, sizeof(int));

        BF_Block_SetDirty(currentBlock);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
    }
    else { //If no split happened return 0.
      BF_Block_SetDirty(currentBlock);
      CALL_BF(BF_UnpinBlock(currentBlock));
      BF_Block_Destroy(&currentBlock);
      return 0;
    }
  }
  //If leaf(data) block:
  else {
    //Find spot to put new item.
    dataPoint += sizeof(char);
    memcpy(&count, dataPoint, sizeof(int));
    dataPoint += sizeof(int);
    int insertionPoint = -1;
    for(int i = 0; i < count; i++) { 
      if(keyCompare(bplus, value1, (void*)(dataPoint)) < 0) {
        insertionPoint = i;
        break;
      }
      dataPoint += sizeOfDataElements;
    }
    //If split necessary.
    if(bplus->dataSize == count) {
      //Make buffer for data cause we lazy(but awesome).
      char* buffer = malloc((bplus->dataSize + 1)*sizeOfDataElements);
      char* bufferPoint = buffer;
      //Copy all elements to buffer and add new one at the end.
      if(insertionPoint == -1) {
        memcpy(bufferPoint, data + sizeof(char) + sizeof(int), sizeOfDataElements*count);
        bufferPoint += sizeOfDataElements*count;
        memcpy(bufferPoint, value1, bplus->attrLength1);
        bufferPoint += bplus->attrLength1;
        memcpy(bufferPoint, value2, bplus->attrLength2);
      }
      else { //In this case the new element has to be placed in between the buffer.
        memcpy(bufferPoint, data + sizeof(char) + sizeof(int), sizeOfDataElements*insertionPoint);
        bufferPoint += sizeOfDataElements*insertionPoint;
        memcpy(bufferPoint, value1, bplus->attrLength1);
        bufferPoint += bplus->attrLength1;
        memcpy(bufferPoint, value2, bplus->attrLength2);
        bufferPoint += bplus->attrLength2;
        char* restData = data + sizeof(char) + sizeof(int) + sizeOfDataElements*insertionPoint;
        memcpy(bufferPoint, restData, sizeOfDataElements*(count - insertionPoint));
      }

      int halfPoint = (count + 1) / 2;

      //Copy first half of buffer to existing block.
      memcpy(data+sizeof(char)+sizeof(int), buffer, halfPoint*sizeOfDataElements);
      //Change original count.
      memcpy(data+sizeof(char), &halfPoint, sizeof(int));

      //Setup first block pointer to second block.
      int previousPointer;
      memcpy(&previousPointer, data + BF_BLOCK_SIZE - sizeof(int), sizeof(int));
      int numOfBlocks;
      CALL_BF(BF_GetBlockCounter(bplus->fd, &numOfBlocks));
      memcpy(data + BF_BLOCK_SIZE - sizeof(int), &numOfBlocks, sizeof(int));

      //Copy second half of buffer to new block.
      BF_Block *newBlock;
      BF_Block_Init(&newBlock);
      CALL_BF(BF_AllocateBlock(bplus->fd, newBlock));
      char* newData = BF_Block_GetData(newBlock);

      //Setup new data block.
      newData[0] = 'D';
      char* newDataPoint = newData + sizeof(char);
      int newCount = (count+1) - halfPoint;
      memcpy(newDataPoint, &newCount, sizeof(int));
      newDataPoint += sizeof(int);
      memcpy(newDataPoint, buffer + halfPoint*sizeOfDataElements,
        newCount*sizeOfDataElements);

      //Setup second block pointer to first block's previous pointer.
      memcpy(newData + BF_BLOCK_SIZE - sizeof(int), &previousPointer, sizeof(int));
      memcpy(returnKey, newDataPoint, bplus->attrLength1);

      BF_Block_SetDirty(currentBlock);
      CALL_BF(BF_UnpinBlock(currentBlock));
      BF_Block_Destroy(&currentBlock);

      BF_Block_SetDirty(newBlock);
      CALL_BF(BF_UnpinBlock(newBlock));
      BF_Block_Destroy(&newBlock);

      free(buffer);
      return numOfBlocks;
    }
    else {
      //Insert at last position.
      if(insertionPoint == -1) {
        memcpy(dataPoint, value1, bplus->attrLength1);
        dataPoint += bplus->attrLength1;
        memcpy(dataPoint, value2, bplus->attrLength2);
        int newCount = count + 1;
        memcpy(data+sizeof(char), &newCount, sizeof(int));
      }
      //Insert between elements.
      else {
        memmove(dataPoint+sizeOfDataElements, dataPoint,
                (count - insertionPoint)*sizeOfDataElements);
        memcpy(dataPoint, value1, bplus->attrLength1);
        dataPoint += bplus->attrLength1;
        memcpy(dataPoint, value2, bplus->attrLength2);
        int newCount = count + 1;
        memcpy(data+sizeof(char), &newCount, sizeof(int));
      }

      BF_Block_SetDirty(currentBlock);
      CALL_BF(BF_UnpinBlock(currentBlock));
      BF_Block_Destroy(&currentBlock);

      return 0;
    }
  }
}

//Find the first block that is needed for the scans in FindNextEntry.
int recurseSearchFirst(openAM* bplus, void* key, int block) {
  BF_Block *currentBlock;
  BF_Block_Init(&currentBlock);
  CALL_BF(BF_GetBlock(bplus->fd, block, currentBlock));
  char* data = BF_Block_GetData(currentBlock);
  int count;
  memcpy(&count, data+sizeof(char), sizeof(int));
  
  //If we're on an index block, find the next block to go to with keyCompare and call the function again. 
  if(data[0] == 'I') {
    data += sizeof(char) + sizeof(int);
    int nextBlockNum;

    for(int i = 0; i < count; i++) {
      memcpy(&nextBlockNum, data, sizeof(int)); 
      if(keyCompare(bplus, key, (void*)(data+sizeof(int))) >= 0) {
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return recurseSearchFirst(bplus, key, nextBlockNum);
      }
      data += bplus->attrLength1 + sizeof(int);
    }

    memcpy(&nextBlockNum, data, sizeof(int));
    CALL_BF(BF_UnpinBlock(currentBlock));
    BF_Block_Destroy(&currentBlock);
    return recurseSearchFirst(bplus, key, nextBlockNum);
  }
  else { //If we reach a data block, we return it.
    CALL_BF(BF_UnpinBlock(currentBlock));
    BF_Block_Destroy(&currentBlock);
    return block;
  }
}

// If something found return 0, else 1.
int findNext(int position, int block, openAM* bplus, scan* currentScan) {
  BF_Block *currentBlock;
  BF_Block_Init(&currentBlock);
  CALL_BF(BF_GetBlock(bplus->fd, block, currentBlock));
  char* data = BF_Block_GetData(currentBlock);
  char* dataPoint;
  int count;
  memcpy(&count, data+sizeof(char), sizeof(int));

  dataPoint = data + sizeof(char) + sizeof(int);

  position++;

  dataPoint += position * (bplus->attrLength1 + bplus->attrLength2);

  //Keep moving in every item in the data block until we find the desired value
  while(position < count) {
    int compareResult = keyCompare(bplus, (void*)dataPoint, currentScan->key);

    if(currentScan->operation == LESS_THAN) {
      if(compareResult < 0) {
        currentScan->blockNumber = block;
        currentScan->positionInBlock = position;
        currentScan->value = malloc(bplus->attrLength2);
        memcpy(currentScan->value, dataPoint+bplus->attrLength1, bplus->attrLength2);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
      else {
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 1;
      }
    }

    else if(currentScan->operation == LESS_THAN_OR_EQUAL) {
      if(compareResult <= 0) {
        currentScan->blockNumber = block;
        currentScan->positionInBlock = position;
        currentScan->value = malloc(bplus->attrLength2);
        memcpy(currentScan->value, dataPoint+bplus->attrLength1, bplus->attrLength2);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
      else {
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 1;
      }
    }

    else if(currentScan->operation == EQUAL) {
      if(compareResult == 0) {
        currentScan->blockNumber = block;
        currentScan->positionInBlock = position;
        currentScan->value = malloc(bplus->attrLength2);
        memcpy(currentScan->value, dataPoint+bplus->attrLength1, bplus->attrLength2);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
      else if(compareResult > 0){
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 1;
      }
    }

    else if(currentScan->operation == GREATER_THAN) {
      if(compareResult > 0) {
        currentScan->blockNumber = block;
        currentScan->positionInBlock = position;
        currentScan->value = malloc(bplus->attrLength2);
        memcpy(currentScan->value, dataPoint+bplus->attrLength1, bplus->attrLength2);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
    }

    else if(currentScan->operation == GREATER_THAN_OR_EQUAL) {
      if(compareResult >= 0) {
        currentScan->blockNumber = block;
        currentScan->positionInBlock = position;
        currentScan->value = malloc(bplus->attrLength2);
        memcpy(currentScan->value, dataPoint+bplus->attrLength1, bplus->attrLength2);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
    }

    else {
      if(compareResult != 0) {
        currentScan->blockNumber = block;
        currentScan->positionInBlock = position;
        currentScan->value = malloc(bplus->attrLength2);
        memcpy(currentScan->value, dataPoint+bplus->attrLength1, bplus->attrLength2);
        CALL_BF(BF_UnpinBlock(currentBlock));
        BF_Block_Destroy(&currentBlock);
        return 0;
      }
    }
    dataPoint += bplus->attrLength1 + bplus->attrLength2;
    position++;
  }

  //Moving to the next block
  int nextBlockNum;
  memcpy(&nextBlockNum, data+BF_BLOCK_SIZE-sizeof(int), sizeof(int));
  if(nextBlockNum != -1) {
    CALL_BF(BF_UnpinBlock(currentBlock));
    BF_Block_Destroy(&currentBlock);
    return findNext(-1, nextBlockNum, bplus, currentScan);
  }
  else { //There are no more data blocks. Return 1 since we didn't find the value. 
    CALL_BF(BF_UnpinBlock(currentBlock));
    BF_Block_Destroy(&currentBlock);
    return 1;
  }
}

//Function that prints every item in the B+ tree. 
//This function isn't called anywhere but it can be used for debugging purposes. (Intended use: AM_printSudo(filename, -1) ) 
int AM_printSudo(int fileDesc, int blockNum) {
  openAM* bplus = tableOfIndexes[fileDesc];
  int fd = bplus->fd;

  if(blockNum == -1)
    blockNum = bplus->root;


  BF_Block *block;
  BF_Block_Init(&block);
  CALL_BF(BF_GetBlock(fd, blockNum, block));
  char* recordData = BF_Block_GetData(block);

  if(recordData[0] == 'I') { //Print every item in an index block.
    printf("\n\nINDEX BLOCK:\n");

    printf("\nPrinting block number: %d\n", blockNum);
    int count;
    memcpy(&count, recordData+sizeof(char), sizeof(int));
    char* recordDataPoint = recordData + sizeof(char) + sizeof(int);
    int pos;

    for(int i = 0; i < count; i++) {
      memcpy(&pos, recordDataPoint, sizeof(int));
      printf("P: %d\t", pos);

      recordDataPoint += sizeof(int);

      if(bplus->attrType1 == 'i') {
        int key;
        memcpy(&key, recordDataPoint, sizeof(int));
        printf("Key: %d\t", key);
      }
      else if(bplus->attrType1 == 'f') {
        float key;
        memcpy(&key, recordDataPoint, sizeof(float));
        printf("Key: %.2f\t", key);
      }
      else {
        char key[255];
        memcpy(&key, recordDataPoint, bplus->attrLength1);
        printf("Key: %s\t", key);
      }

      recordDataPoint += bplus->attrLength1;
    }
    memcpy(&pos, recordDataPoint, sizeof(int));
    printf("P: %d\n", pos);


    recordDataPoint = recordData + sizeof(char) + sizeof(int);
    for(int i = 0; i < count; i++) {
      memcpy(&pos, recordDataPoint, sizeof(int));
      recordDataPoint += sizeof(int);
      recordDataPoint += bplus->attrLength1;
      AM_printSudo(fileDesc, pos);
    }
    memcpy(&pos, recordDataPoint, sizeof(int));
    AM_printSudo(fileDesc, pos);
  }
  else { //Print every item in a data block.
    printf("\n\nDATA BLOCK:\n");

    printf("\nPrinting block number: %d\n", blockNum);

    int count;
    memcpy(&count, recordData+sizeof(char), sizeof(int));
    char* recordDataPoint = recordData + sizeof(char) + sizeof(int);
    for(int i = 0; i < count; i++) {
      if(bplus->attrType1 == 'i') {
        int key;
        memcpy(&key, recordDataPoint, sizeof(int));
        printf("Key: %d\n", key);
      }
      else if(bplus->attrType1 == 'f') {
        float key;
        memcpy(&key, recordDataPoint, sizeof(float));
        printf("Key: %.2f\n", key);
      }
      else {
        char key[255];
        memcpy(&key, recordDataPoint, bplus->attrLength1);
        printf("Key: %s\n", key);
      }

      recordDataPoint += bplus->attrLength1;

      if(bplus->attrType2 == 'i') {
        int value;
        memcpy(&value, recordDataPoint, sizeof(int));
        printf("Value: %d\n", value);
      }
      else if(bplus->attrType2 == 'f') {
        float value;
        memcpy(&value, recordDataPoint, sizeof(float));
        printf("Value: %.2f\n", value);
      }
      else {
        char value[255];
        memcpy(&value, recordDataPoint, bplus->attrLength2);
        printf("Value: %s\n", value);
      }

      recordDataPoint += bplus->attrLength2;
    }
  }

  CALL_BF(BF_UnpinBlock(block));
  BF_Block_Destroy(&block);
  

  return AME_OK;
}

//AM_Init() sets every entry of our global tables to NULL and initializes the BF block level
void AM_Init() {
  
  for(int i = 0; i < MAX_OPEN_FILES; i++) {
      tableOfIndexes[i] = NULL;
  }
  
  for(int i = 0; i < MAXSCANS; i++) {
      tableOfScans[i] = NULL;
  }
  
  BF_Init(LRU);
	
  return;
}

//Creating a new file.
int AM_CreateIndex(char *fileName, char attrType1, 
	                int attrLength1, char attrType2,
                  int attrLength2) {
  //Checking if the attribute values are valid.
  if(attrType1 == 'c'){
    if(attrLength1 > 255 || attrLength1 < 1){
      AM_errno = AME_PARAMETER;
      return AM_errno; 
    }
  }
  else if(attrType1 == 'i'){
    if(attrLength1 != sizeof(int)){
      AM_errno = AME_PARAMETER;
      return AM_errno;
    }
  }
  else if(attrType1 == 'f'){
    if(attrLength1 != sizeof(float)) {
      AM_errno = AME_PARAMETER;
      return AM_errno;
    }
  }
  else {
    AM_errno = AME_PARAMETER;
    return AM_errno;
  }

  if(attrType2 == 'c'){
    if(attrLength2 > 255 || attrLength2 < 1){
      AM_errno = AME_PARAMETER;
      return AM_errno; 
    }
  }
  else if(attrType2 == 'i'){
    if(attrLength2 != sizeof(int)){
      AM_errno = AME_PARAMETER;
      return AM_errno;
    }
  }
  else if(attrType2 == 'f' ){
    if(attrLength2 != sizeof(float)){
      AM_errno = AME_PARAMETER;
      return AM_errno;
    }
  }
  else {
    AM_errno = AME_PARAMETER;
      return AM_errno;
  }                   

  
  // File must not already exist.
	if(access(fileName, F_OK) == 0){
		printf("File already exists. \n");
    AM_errno = AME_EXISTS;
    return AM_errno;
	}
  
  int fd;
  CALL_BF(BF_CreateFile(fileName));
  CALL_BF(BF_OpenFile(fileName, &fd));

  // Create first block.
  BF_Block *block;
  char* data;
  BF_Block_Init(&block);
  CALL_BF(BF_AllocateBlock(fd, block));
  data = BF_Block_GetData(block);

  // Write "BP" in the beginning of first block to signify
  // we have an B+ tree file.
  char str1[3];
  strcpy(str1, "BP");
  memcpy(data, str1, 2);

  //If root == 0, there is no root yet.
  int root = 0;

  // Write attributes in the first block.
  char* dataPoint = data+2;
  memcpy(dataPoint, &attrType1, sizeof(char));
  dataPoint += sizeof(char);
  memcpy(dataPoint, &attrLength1, sizeof(int));
  dataPoint += sizeof(int);
  memcpy(dataPoint, &attrType2, sizeof(char));
  dataPoint += sizeof(char);
  memcpy(dataPoint, &attrLength2, sizeof(int));
  dataPoint += sizeof(int);
  memcpy(dataPoint, &root, sizeof(int));

  // Save changes to first block.
  BF_Block_SetDirty(block);
  CALL_BF(BF_UnpinBlock(block));

  // Make changes and close BP file.
  BF_Block_Destroy(&block);
  CALL_BF(BF_CloseFile(fd));

  return AME_OK;
}

//Remove function (from unistd.h) is used to remove the file from the disk.
int AM_DestroyIndex(char *fileName) {
  
  int status = remove(fileName);
  
  if(status == 0 ) {
    return AME_OK;
  }
  else {
    printf("Error in deletion \n");
    AM_errno = AME_DELETE_ERROR;
    return AM_errno; 
  }

}

//Opens a file index.
int AM_OpenIndex(char *fileName) {
  char attrType1;
	int attrLength1;
  char attrType2;
  int attrLength2;
  int root;

  // If we have reached max open files, return error.
  if(openFiles == MAX_OPEN_FILES) {
    fprintf(stderr, "Can't open more files.\n");
    AM_errno = AME_MAX_FILES;
    return AM_errno;
  }

  openFiles++;

  // Open file.
  int fd;
  CALL_BF(BF_OpenFile(fileName, &fd));

  // Init block.
  BF_Block *block;
  BF_Block_Init(&block);
  char* data;
  CALL_BF(BF_GetBlock(fd, 0, block));
  data = BF_Block_GetData(block);

  // Check if it's not B+ file and close it.
  if(strncmp(data, "BP", 2) != 0) {
    CALL_BF(BF_UnpinBlock(block));
    BF_Block_Destroy(&block);
    CALL_BF(BF_CloseFile(fd));
    AM_errno = AME_NOT_BP;
    return AM_errno;
  }

  // Read attributes.
  char* dataPoint = data+2;
  memcpy(&attrType1, dataPoint, sizeof(char));
  dataPoint += sizeof(char);
  memcpy(&attrLength1, dataPoint, sizeof(int));
  dataPoint += sizeof(int);
  memcpy(&attrType2, dataPoint, sizeof(char));
  dataPoint += sizeof(char);
  memcpy(&attrLength2, dataPoint, sizeof(int));
  dataPoint += sizeof(int);
  memcpy(&root, dataPoint, sizeof(int));
  
  //Find first empty position in table of indexes and put the new openAM struct.
  int index = 0;
  while(index < MAX_OPEN_FILES) {
    if(tableOfIndexes[index] == NULL) {
      tableOfIndexes[index] = malloc(sizeof(openAM));
      tableOfIndexes[index]->fd = fd;
      tableOfIndexes[index]->attrType1 = attrType1;
      tableOfIndexes[index]->attrLength1 = attrLength1;
      tableOfIndexes[index]->attrType2 = attrType2;
      tableOfIndexes[index]->attrLength2 = attrLength2;
      tableOfIndexes[index]->root = root;
      tableOfIndexes[index]->indexSize = howManyInIndexBlock(tableOfIndexes[index]); //Size of key and block number
      tableOfIndexes[index]->dataSize = howManyInDataBlock(tableOfIndexes[index]); //Size of key and value
      break;
    }
    index++;
  }
  
  // Close block.
  CALL_BF(BF_UnpinBlock(block));
  BF_Block_Destroy(&block);

  return index;
}

//Find the index in the table of indexes and close the desired file. 
int AM_CloseIndex(int fileDesc) {
  
  openAM* indexToDie = tableOfIndexes[fileDesc];
  CALL_BF(BF_CloseFile(indexToDie->fd));
  free(indexToDie);
  tableOfIndexes[fileDesc] = NULL;
  openFiles--;
  
  return AME_OK;
}

//This function adds a new entry in our B+ tree.
int AM_InsertEntry(int fileDesc, void *value1, void *value2) {
  openAM* bplus = tableOfIndexes[fileDesc];
  int fd = bplus->fd;

  int insert_sucess = 0;

  //If empty, create root (one leaf).
  if(bplus->root == 0) {
    BF_Block *dataBlock;
    BF_Block_Init(&dataBlock);
    char* recordData;
    CALL_BF(BF_AllocateBlock(bplus->fd, dataBlock));
    recordData = BF_Block_GetData(dataBlock);

    recordData[0] = 'D';
    char* recordDataPoint = recordData + sizeof(char);

    int count = 1;
    memcpy(recordDataPoint, &count, sizeof(int));

    recordDataPoint += sizeof(int);
    memcpy(recordDataPoint, value1, bplus->attrLength1);
    recordDataPoint += bplus->attrLength1;
    memcpy(recordDataPoint, value2, bplus->attrLength2);

    int next = -1; //Default value of the next block number is -1
    memcpy(recordData + BF_BLOCK_SIZE - sizeof(int), &next, sizeof(int));

    BF_Block_SetDirty(dataBlock);
    CALL_BF(BF_UnpinBlock(dataBlock));
    BF_Block_Destroy(&dataBlock);

    //Update the value of the root in the first block (the one with the metadata)
    BF_Block *firstBlock;
    BF_Block_Init(&firstBlock);
    char* firstData;
    CALL_BF(BF_GetBlock(bplus->fd, 0, firstBlock));
    firstData = BF_Block_GetData(firstBlock);
    firstData += 2 + 2*(sizeof(char) + sizeof(int));

    bplus->root = 1;

    memcpy(firstData, &bplus->root, sizeof(int));
    BF_Block_SetDirty(firstBlock);
    CALL_BF(BF_UnpinBlock(firstBlock));
    BF_Block_Destroy(&firstBlock);

    insert_sucess = 1;
  }
  //Else, go through B+ tree using the recursiveInsert function.
  else {
    void* returnKey = malloc(bplus->attrLength1);
    int newBlock = recursiveInsert(bplus, bplus->root, value1, value2, returnKey);
    if(newBlock != 0) { //If split occurred we have to create a new root.
      BF_Block *indexBlock;
      BF_Block_Init(&indexBlock);
      char* recordData;
      CALL_BF(BF_AllocateBlock(bplus->fd, indexBlock)); 
      recordData = BF_Block_GetData(indexBlock);

      recordData[0] = 'I';
      char* recordDataPoint = recordData + sizeof(char);

      int count = 1;
      memcpy(recordDataPoint, &count, sizeof(int));

      //Left pointer will point to the previous root and right pointer will point to the new block that was reutrned.
      //returnKey's value is preserved among the recursive function calls.
      recordDataPoint += sizeof(int);
      memcpy(recordDataPoint, &bplus->root, sizeof(int));
      recordDataPoint += sizeof(int);
      memcpy(recordDataPoint, returnKey, bplus->attrLength1);
      recordDataPoint += bplus->attrLength1;
      memcpy(recordDataPoint, &newBlock, sizeof(int));

      int newRootBlock;
      CALL_BF(BF_GetBlockCounter(bplus->fd, &newRootBlock));
      newRootBlock--;
      
      //Update the value of the root in the first block (the one with the metadata)
      BF_Block *firstBlock;
      BF_Block_Init(&firstBlock);
      char* firstData;
      CALL_BF(BF_GetBlock(bplus->fd, 0, firstBlock));
      firstData = BF_Block_GetData(firstBlock);
      firstData += 2 + 2*(sizeof(char) + sizeof(int));

      bplus->root = newRootBlock;

      memcpy(firstData, &bplus->root, sizeof(int));
      BF_Block_SetDirty(firstBlock);
      CALL_BF(BF_UnpinBlock(firstBlock));
      BF_Block_Destroy(&firstBlock);

      BF_Block_SetDirty(indexBlock);
      CALL_BF(BF_UnpinBlock(indexBlock));
      BF_Block_Destroy(&indexBlock);
    }
    free(returnKey);

    insert_sucess = 1;
  }

  if (insert_sucess == 1){
    return AME_OK;
  }
  else{
    AM_errno = AME_INSERT_ERROR;
    return AM_errno;
  }
}

//Creates a new scan.
int AM_OpenIndexScan(int fileDesc, int op, void *value) {
  // If we have reached max open scans, return error.
  if(openScans == MAX_OPEN_FILES) {
    fprintf(stderr, "Can't open more scans.\n");
    AM_errno = AME_MAX_SCANS;
    return AM_errno;
  }

  //Find first empty position in table of open scans.
  int index = 0;
  while(index < MAXSCANS) {
    if(tableOfScans[index] == NULL) {
      tableOfScans[index] = malloc(sizeof(scan));
      tableOfScans[index]->openAM = fileDesc;
      tableOfScans[index]->operation = op;
      tableOfScans[index]->blockNumber = -1;
      tableOfScans[index]->positionInBlock = -1;
      tableOfScans[index]->value = NULL;
      tableOfScans[index]->key = malloc(tableOfIndexes[fileDesc]->attrLength1);
      memcpy(tableOfScans[index]->key, value, tableOfIndexes[fileDesc]->attrLength1);
      break;
    }
    index++;
  }
  openScans++;

  //Here we find our first block where the search in AM_FindNextEntry will begin.
  int firstBlock;

  //For these operations we know we'll have to begin in the first created block
  if(op == LESS_THAN || op == LESS_THAN_OR_EQUAL || op == NOT_EQUAL)
    firstBlock = 1;
  else //Otherwise we find the first desired block with the use of recurseSearchFirst function
    firstBlock = recurseSearchFirst(tableOfIndexes[fileDesc], value, tableOfIndexes[fileDesc]->root);

  tableOfScans[index]->blockNumber = firstBlock;
  return index;
}

//Returns the value we need using the findNext function
void *AM_FindNextEntry(int scanDesc) {
  scan* currentScan = tableOfScans[scanDesc];
  openAM* bplus = tableOfIndexes[currentScan->openAM];

  //Free any previously allocated value
  if (currentScan->value != NULL){
    free(currentScan->value);
    currentScan->value = NULL;
  }

	int found = findNext(currentScan->positionInBlock, currentScan->blockNumber, bplus, currentScan);
  if(found == 0) {
    return currentScan->value;
  }

  AM_errno = AME_EOF;
  return NULL;
}

//Frees the opened scan and any other allocated memory
int AM_CloseIndexScan(int scanDesc) {

  if(tableOfScans[scanDesc]->value != NULL) {
    free(tableOfScans[scanDesc]->value);
  }

  free(tableOfScans[scanDesc]->key);
  free(tableOfScans[scanDesc]);
  tableOfScans[scanDesc] = NULL;
  openScans--;
  return AME_OK;
}

//Printing the string in the errString argument as well as the error corresponding to the AM_errno variable.
void AM_PrintError(char *errString) {
  
  printf("%s\n", errString);

  if(AM_errno == AME_EOF)
    printf("AM_EOF\n");
  else if(AM_errno == AME_PARAMETER)
    printf("AME_PARAMETER\n");
  else if(AM_errno == AME_EXISTS)
    printf("AME_EXISTS\n");
  else if(AM_errno == AME_MAX_FILES)
    printf("AME_MAX_FILES\n");
  else if(AM_errno == AME_NOT_BP)
    printf("AME_NOT_BP\n");
  else if(AM_errno == AME_MAX_SCANS)
    printf("AME_MAX_SCANS\n");
  else if(AM_errno == AME_INVALID_TYPE)
    printf("AME_INVALID_TYPE\n");
  else if(AM_errno == AME_DELETE_ERROR)
    printf("AME_AME_DELETE_ERROR\n");
  else if(AM_errno == AME_INSERT_ERROR)
    printf("AME_INSERT_ERROR\n");  
  else if(AM_errno == BF_BLOCK_LEVEL_ERROR)
    printf("BF_BLOCK_LEVEL_ERROR\n");
  
}

//Closing the BF block level.
void AM_Close() {
  BF_Close();
}
