// Host-side .cpmx dump tool for the conformance test.
//
// Decodes a .cpmx file with the FIRMWARE reader (lib/Cpmx/CpmxReader) and
// prints every value as canonical JSON with raw fixed-point integers —
// the same shape converter/cpmx_dump.py produces from the Python reference
// reader. test/conformance/run_conformance.py compares the two.
//
// Exit codes: 0 = decoded fully, 1 = rejected (invalid/corrupt file).

#include <CpmxReader.h>

#include <cstdio>
#include <string>

namespace {

void printHexString(const char* s) {
  printf("\"");
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
    printf("%02x", *p);
  }
  printf("\"");
}

bool printPrim(const cpmx::Prim& p) {
  using cpmx::PrimType;
  switch (p.type) {
    case PrimType::Glyph:
      printf("{\"t\":1,\"cp\":%u,\"x\":%d,\"y\":%d,\"s\":%u}", p.codepoint, p.x1, p.y1, p.scale);
      return true;
    case PrimType::Line:
      printf("{\"t\":2,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"w\":%u}", p.x1, p.y1, p.x2, p.y2, p.w);
      return true;
    case PrimType::Beam:
    case PrimType::Curve:
    case PrimType::Polyline: {
      const int tag = p.type == PrimType::Beam ? 3 : (p.type == PrimType::Curve ? 4 : 5);
      if (tag == 3) {
        printf("{\"t\":3,\"pts\":[");
      } else {
        printf("{\"t\":%d,\"w\":%u,\"pts\":[", tag, p.w);
      }
      for (uint8_t i = 0; i < p.pointCount; i++) {
        int16_t x, y;
        p.pointAt(i, x, y);
        printf("%s[%d,%d]", i ? "," : "", x, y);
      }
      printf("]}");
      return true;
    }
    case PrimType::Dot:
      printf("{\"t\":6,\"x\":%d,\"y\":%d,\"r\":%u}", p.x1, p.y1, p.w);
      return true;
    case PrimType::Rect:
      printf("{\"t\":7,\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u}", p.x1, p.y1, p.w, p.h);
      return true;
    case PrimType::Text: {
      printf("{\"t\":8,\"x\":%d,\"y\":%d,\"size\":%u,\"flags\":%u,\"text\":\"", p.x1, p.y1, p.w, p.textFlags);
      for (uint8_t i = 0; i < p.textLen; i++) {
        printf("%02x", static_cast<unsigned char>(p.text[i]));
      }
      printf("\"}");
      return true;
    }
  }
  return false;
}

bool printPrimList(cpmx::PrimList list) {
  printf("[");
  cpmx::Prim prim;
  bool first = true;
  uint16_t expected = list.count;
  while (list.next(prim)) {
    if (!first) printf(",");
    first = false;
    if (!printPrim(prim)) return false;
    expected--;
  }
  printf("]");
  return expected == 0;  // clean exhaustion, not a decode error
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: cpmx_dump <file.cpmx>\n");
    return 2;
  }
  cpmx::CpmxReader reader;
  if (!reader.open(argv[1])) {
    return 1;
  }

  printf("{\"title\":");
  printHexString(reader.title());
  printf(",\"composer\":");
  printHexString(reader.composer());
  printf(",\"arranger\":");
  printHexString(reader.arranger());
  // unitsPerStaffSpace is not exposed by the firmware reader (unused on
  // device); read it straight from the header for the dump.
  {
    FILE* f = fopen(argv[1], "rb");
    unsigned char hdr[14] = {0};
    if (!f || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
      if (f) fclose(f);
      return 1;
    }
    fclose(f);
    printf(",\"units\":%u", hdr[12] | (hdr[13] << 8));
  }

  printf(",\"playOrder\":[");
  for (uint16_t i = 0; i < reader.playOrderLength(); i++) {
    printf("%s%u", i ? "," : "", reader.playOrderAt(i));
  }
  printf("]");

  printf(",\"blocks\":[");
  for (uint8_t b = 0; b < reader.headerBlockCount(); b++) {
    cpmx::HeaderBlockView block;
    if (!reader.loadHeaderBlock(b, block)) {
      return 1;
    }
    printf("%s{\"advance\":%u,\"prims\":", b ? "," : "", block.advanceFp);
    if (!printPrimList(block.prims)) return 1;
    printf("}");
  }
  printf("]");

  printf(",\"measures\":[");
  for (uint16_t m = 0; m < reader.measureCount(); m++) {
    cpmx::MeasureView view;
    if (!reader.loadMeasure(m, view)) {
      return 1;
    }
    printf("%s{\"width\":%u,\"yMin\":%d,\"yMax\":%d,\"headerIdx\":%u,\"beatsX8\":%u,\"prims\":", m ? "," : "",
           view.widthFp, view.yMinFp, view.yMaxFp, view.headerIdx, view.beatsX8);
    if (!printPrimList(view.prims)) return 1;
    printf(",\"splitStart\":");
    if (!printPrimList(view.splitStart)) return 1;
    printf(",\"splitEnd\":");
    if (!printPrimList(view.splitEnd)) return 1;
    printf("}");
  }
  printf("]}\n");
  return 0;
}
