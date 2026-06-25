#pragma once
#include "common.h"
#include "PageCache.h"
#include "ObjectPool.h"
#include "ThreadCache.h"

// 线程申请空间接口
void *tcmalloc(size_t size);

// 线程回收空间接口
// ptr - 回收空间地址
// size - 回收空间字节数
void tcfree(void *ptr);
