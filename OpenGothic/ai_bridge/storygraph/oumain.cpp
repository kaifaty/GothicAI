#include "oumessages.h"

#include <cstdio>
#include <stdexcept>

#ifdef _MSC_VER
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if(argc!=3) {
    std::fputs("usage: storygraph-ou <prepared base> --ou-messages\n",stderr);
    return 2;
    }
  try {
    if(std::filesystem::path(argv[2])=="--ou-messages")
      return storygraph::printPreparedOuMessages(argv[1]);
    if(std::filesystem::path(argv[1])=="--ou-messages")
      return storygraph::printPreparedOuMessages(argv[2]);
    std::fputs("usage: storygraph-ou <prepared base> --ou-messages\n",stderr);
    return 2;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"storygraph: %s\n",error.what());
    return 1;
    }
  }
