"""A failed stage must be visible and must not become a reconnect timeout."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class StageDiagnosticsTest(unittest.TestCase):
    def test_fragmented_error_terminals(self):
        source = (ROOT / "libs/h2loader_host/src/h2_h2loader_host_serial.c").read_text()
        start = source.index("typedef struct serial_stage_output {")
        end = source.index("h2_pal_result_t h2_h2loader_host_serial_stage(", start)
        stub = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef int h2_pal_result_t;
#define H2_PAL_OK 0
#define H2_PAL_ERR_IO -4
typedef struct {
 int (*on_log)(void *, const uint8_t *, size_t);
 void *log_user;
} h2_h2loader_host_serial_connection_t;
static size_t logged;
static int log_result;
static int log_output(void *user,const uint8_t *data,size_t len) {
 (void)user; (void)data; logged += len; return log_result;
}
'''
        main = r'''
int main(void) {
 h2_h2loader_host_serial_connection_t c = {.on_log=log_output};
 const char *errors[] = {
  "H2_LOADER_STAGE_RECEIVE result=fail code=-12\n",
  "H2_LOADER_STAGE result=fail code=-4\n"
 };
 for (size_t e=0;e<2;e++) {
  const uint8_t *data=(const uint8_t *)errors[e];
  size_t n=strlen(errors[e]);
  for (size_t split=0;split<n;split++) {
   serial_stage_output_t o={.connection=&c}; logged=0;
   assert(serial_stage_output(&o,data,split)==0);
   assert(serial_stage_output(&o,data+split,n-split)==-4);
   assert(logged==n);
  }
 }
 serial_stage_output_t o={.connection=&c};
 const char *ok="H2_LOADER_STAGE_RECEIVE result=OK code=0\r\nH2_LOADER_STAGE result=OK code=0\n";
 for (size_t i=0;i<strlen(ok);i++)
  assert(serial_stage_output(&o,(const uint8_t *)ok+i,1)==0);
 uint8_t long_line[512]; memset(long_line,'x',sizeof(long_line));
 assert(serial_stage_output(&o,long_line,sizeof(long_line))==0);
 assert(serial_stage_output(&o,(const uint8_t *)"\n",1)==0);
 c.on_log=NULL;
 assert(serial_stage_output(&o,(const uint8_t *)errors[0],strlen(errors[0]))==-4);
 o=(serial_stage_output_t){.connection=&c}; c.on_log=log_output; log_result=-7;
 assert(serial_stage_output(&o,(const uint8_t *)ok,strlen(ok))==-7);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-stage-diagnostics-") as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + source[start:end] + main)
            subprocess.run(["cc", "-std=c11", str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
