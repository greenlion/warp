#include <cstdint>
#include <cassert>
#include "../sparse.hpp"

void set_and_test_bits(sparsebitmap* t) {
  int errcode=0;
  for(int i=256;i>0;--i) {
    //std::cout << "setting bit: " << i << "\n";
    if((errcode = t->set_bit(i)) != 0) {
      std::cout << " NO OK | set_bit returned: "  << errcode << "\n";
      exit(abs(errcode));
    }
    
    for(int n=1;n<=256;++n) {
      int is_set = t->is_set(n);
      if(n >= i) {
        if(is_set != 1) {
          std::cout << "NOT OK | n: " << n << " i: " << i << ", is_set: " << is_set<< " expected 1\n";
        } 
      } else {
        if(is_set == 1) {
          std::cout << "NOT OK | n: " << n << " i: " << i << ", is_set: " << is_set<< " expected 0\n";
        } 
      }
    }
  } 
  
  for(int i=1;i<=256;++i) {
    if(!t->is_set(i)) {
        std::cout << "NOT OK | i: " << i << ", is_set: 0, expected 1\n";
        exit(errcode);
        exit(i);
    }
  }
}

void test_last_set_bit() {
  unlink("last.bitmap");
  unlink("last.bitmap.txlog");
  sparsebitmap *t = new sparsebitmap("last.bitmap");
  uint64_t last = 99;
  int rc = t->get_last_set_bit(last);
  if(rc != -1) {
    std::cout << "NOT OK | empty bitmap: get_last_set_bit returned " << rc << ", expected -1\n";
    exit(1);
  }
  // includes block boundaries (64, 192/193) where offset 0 holds bit 64
  int bits[] = {5, 64, 70, 130, 192, 193};
  for(int b : bits) {
    if(t->set_bit(b) != 0) {
      std::cout << "NOT OK | set_bit(" << b << ") failed\n";
      exit(1);
    }
    rc = t->get_last_set_bit(last);
    if(rc != 0 || last != (uint64_t)b) {
      std::cout << "NOT OK | after set_bit(" << b << "): rc: " << rc << " last: " << last << "\n";
      exit(1);
    }
  }
  t->commit();
  t->close(1);
  delete t;
  unlink("last.bitmap");
}

int main(int argc, char** argv) {
  unlink("test.bitmap");
  unlink("test.bitmap.txlog");
  //bitmap_debug = false;
  sparsebitmap *t = new sparsebitmap("test.bitmap");
  set_and_test_bits(t);
  t->commit();
  t->close(1);
  delete t;
  
  // should recover the bitmap
   t = new sparsebitmap("test.bitmap");

  if(!t->is_set(1)) {
    std::cout << "bit 1 must be set!";
    exit(1);
  }

  if(!t->is_set(256)) {
    std::cout << "bit 1 must be set!";
    exit(1);
  }
  uint64_t last = 0;
  if(t->get_last_set_bit(last) != 0 || last != 256) {
    std::cout << "NOT OK | recovered bitmap: last set bit is " << last << ", expected 256\n";
    exit(1);
  }

  test_last_set_bit();
  std::cout << "OK\n";
  exit(0);
  return 0;
}
