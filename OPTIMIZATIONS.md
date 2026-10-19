# Optimizations for Kairos VM

This document describes the 5 optimizations implemented in the Kairos VM to improve performance while maintaining byte-by-byte compatibility, identical error messages, and control flow.

## 1. Thread-local caching for `make_frame_key_par_rec`

**Problem**: The `make_frame_key_par_rec` function was creating the same frame keys repeatedly for concurrent threads working on the same procedure at the same depth, causing unnecessary string operations and memory allocation.

**Solution**: Added thread-local caching to avoid recomputing identical keys. When called with the same parameters consecutively, it returns a cached key instead of computing it again.

**Impact**: Eliminates redundant computations for repeated calls within the same thread.

## 2. Frame Cloning Optimization with thread-local caching

**Problem**: The `clone_frame_for_depth` function was repeatedly creating frames for the same procedure and depth combination without reusing previously created frames.

**Solution**: Implemented thread-local caching to reuse frames when possible, reducing allocation overhead.

**Impact**: Significant reduction in frame creation overhead for recursive procedures.

## 3. Call/Uncall Caching with thread-local storage

**Problem**: The `vm_call_cache` function was repeatedly parsing the same call lines without proper caching.

**Solution**: Implemented thread-local caching to store previously computed call slots, avoiding redundant parsing.

**Impact**: Improved performance for frequently called procedures.

## 4. Frame Reuse Strategy with thread-local frame caching

**Problem**: Creating new frames when existing ones could be reused was inefficient.

**Solution**: Added a frame reuse strategy that caches frames for later use instead of creating new ones every time.

**Impact**: Reduced memory allocation and improved performance for frequently used frames.

## 5. Thread-Local Frame Indexing with proper cache management

**Problem**: The `clone_frame_for_thread` function wasn't properly utilizing thread-local caching to avoid redundant lookups.

**Solution**: Enhanced caching logic to maintain thread-local frame references for faster access.

**Impact**: Reduced lookup time and improved performance for thread-specific frame operations.

All optimizations maintain complete byte-for-byte compatibility, identical error messages, and control flow as required.