#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace model
{

// ---- palette --------------------------------------------------------------

namespace col
{
const uint32_t Reserved = rgba(55, 55, 68);
const uint32_t NoAccess = rgba(45, 45, 52);
const uint32_t ImageExec = rgba(158, 96, 214);   // code
const uint32_t ImageRO = rgba(92, 112, 200);     // const data
const uint32_t ImageRW = rgba(120, 140, 224);    // writable image data
const uint32_t Mapped = rgba(56, 170, 168);      // mapped files / shared
const uint32_t PrivateExec = rgba(220, 112, 60); // JIT / RWX
const uint32_t PrivateRW = rgba(74, 182, 96);    // heap / stack
const uint32_t PrivateRO = rgba(96, 152, 120);
const uint32_t Guard = rgba(210, 196, 70);
// content mode
const uint32_t Unsampled = rgba(72, 72, 84);
const uint32_t Unreadable = rgba(70, 34, 34);
const uint32_t Zeroed = rgba(24, 24, 30);
} // namespace col

static bool hasExec(DWORD protect)
{
   DWORD b = protect & 0xFF;
   return b == PAGE_EXECUTE || b == PAGE_EXECUTE_READ || b == PAGE_EXECUTE_READWRITE ||
          b == PAGE_EXECUTE_WRITECOPY;
}
static bool hasWrite(DWORD protect)
{
   DWORD b = protect & 0xFF;
   return b == PAGE_READWRITE || b == PAGE_WRITECOPY || b == PAGE_EXECUTE_READWRITE ||
          b == PAGE_EXECUTE_WRITECOPY;
}

static uint32_t metadataColor(const memscan::Region& r)
{
   using memscan::RegionState;
   if (r.state == RegionState::Reserved)
   {
      return col::Reserved;
   }
   if (r.state != RegionState::Committed)
   {
      return col::Reserved;
   }

   if (r.protect & PAGE_GUARD)
   {
      return col::Guard;
   }
   if ((r.protect & 0xFF) == PAGE_NOACCESS || r.protect == 0)
   {
      return col::NoAccess;
   }

   const bool ex = hasExec(r.protect);
   const bool wr = hasWrite(r.protect);
   switch (r.type)
   {
   case MEM_IMAGE:
      if (ex)
      {
         return col::ImageExec;
      }
      return wr ? col::ImageRW : col::ImageRO;
   case MEM_MAPPED:
      return col::Mapped;
   default: // MEM_PRIVATE
      if (ex)
      {
         return col::PrivateExec;
      }
      return wr ? col::PrivateRW : col::PrivateRO;
   }
}

std::vector<LegendItem> metadataLegend()
{
   return {
      {"Image exec (code)", col::ImageExec},
      {"Image read-only", col::ImageRO},
      {"Image writable", col::ImageRW},
      {"Mapped / shared", col::Mapped},
      {"Private exec (RWX)", col::PrivateExec},
      {"Private writable", col::PrivateRW},
      {"Private read-only", col::PrivateRO},
      {"Guard page", col::Guard},
      {"No access", col::NoAccess},
      {"Reserved", col::Reserved},
   };
}

std::vector<LegendItem> contentLegend()
{
   return {
      {"Low entropy", rgba(40, 90, 200)},  {"Medium entropy", rgba(70, 190, 90)},
      {"High entropy", rgba(220, 70, 60)}, {"Zeroed", col::Zeroed},
      {"Unreadable", col::Unreadable},     {"Not sampled", col::Unsampled},
      {"Reserved", col::Reserved},         {"Changed", rgba(255, 244, 120)},
   };
}

static uint32_t entropyColor(double e)
{
   // blue -> green -> red heat ramp
   e = std::clamp(e, 0.0, 1.0);
   double r, g, b;
   if (e < 0.5)
   {
      double t = e / 0.5; // blue -> green
      r = 40 + t * (70 - 40);
      g = 90 + t * (190 - 90);
      b = 200 + t * (90 - 200);
   }
   else
   {
      double t = (e - 0.5) / 0.5; // green -> red
      r = 70 + t * (220 - 70);
      g = 190 + t * (70 - 190);
      b = 90 + t * (60 - 90);
   }
   return rgba((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// ---- grid construction ----------------------------------------------------

std::shared_ptr<Grid> buildGrid(const std::vector<memscan::Region>& all, int width)
{
   auto g = std::make_shared<Grid>();
   if (width < 16)
   {
      width = 16;
   }
   g->width = width;
   g->mode = ColorMode::Metadata;

   // Keep reserved + committed, in address order.
   for (const auto& r : all)
   {
      if (r.state == memscan::RegionState::Free)
      {
         continue;
      }
      g->regions.push_back(r);
      if (r.state == memscan::RegionState::Committed)
      {
         g->stats.committedBytes += r.size;
      }
      else
      {
         g->stats.reservedBytes += r.size;
      }
   }
   g->stats.regionCount = g->regions.size();

   // Choose aggregation so the grid fits within the max texture height.
   auto countCells = [&](size_t bpc)
   {
      size_t cellBytes = BLOCK_SIZE * bpc;
      size_t total = 0;
      for (const auto& r : g->regions)
      {
         total += (r.size + cellBytes - 1) / cellBytes;
      }
      return total;
   };

   size_t bpc = 1;
   size_t total = countCells(bpc);
   size_t maxCells = (size_t)width * MAX_TEX_DIM;
   while (total > maxCells)
   {
      bpc *= 2;
      total = countCells(bpc);
   }
   g->blocksPerCell = bpc;
   g->totalCells = total;

   // Prefix sums (cell start per region).
   size_t cellBytes = g->cellBytes();
   g->regionCellStart.resize(g->regions.size() + 1);
   size_t acc = 0;
   for (size_t i = 0; i < g->regions.size(); ++i)
   {
      g->regionCellStart[i] = acc;
      acc += (g->regions[i].size + cellBytes - 1) / cellBytes;
   }
   g->regionCellStart[g->regions.size()] = acc;

   g->height = (int)((total + width - 1) / width);
   if (g->height < 1)
   {
      g->height = 1;
   }

   g->pixels.assign((size_t)g->width * g->height, rgba(18, 18, 22));

   // Metadata colors: constant per region.
   for (size_t ri = 0; ri < g->regions.size(); ++ri)
   {
      uint32_t c = metadataColor(g->regions[ri]);
      size_t start = g->regionCellStart[ri];
      size_t end = g->regionCellStart[ri + 1];
      for (size_t i = start; i < end; ++i)
      {
         g->pixels[i] = c; // linear layout == row-major fill
      }
   }
   return g;
}

BlockMetrics analyzeBytes(const uint8_t* data, size_t n)
{
   BlockMetrics m;
   if (!data || n == 0)
   {
      return m;
   }
   m.readable = true;
   size_t hist[256] = {0};
   for (size_t i = 0; i < n; ++i)
   {
      hist[data[i]]++;
   }
   double ent = 0.0;
   for (int i = 0; i < 256; ++i)
   {
      if (!hist[i])
      {
         continue;
      }
      double p = (double)hist[i] / (double)n;
      ent -= p * std::log2(p);
   }
   m.entropy = ent / 8.0; // normalize (max 8 bits)
   m.zeroRatio = (double)hist[0] / (double)n;
   return m;
}

static uint32_t checksum(const uint8_t* data, size_t n)
{
   // Cheap FNV-1a; 0 is reserved as "no data".
   uint32_t h = 2166136261u;
   for (size_t i = 0; i < n; ++i)
   {
      h ^= data[i];
      h *= 16777619u;
   }
   return h ? h : 1u;
}

void fillContent(HANDLE h, Grid& g, uint64_t byteBudget)
{
   g.mode = ColorMode::Content;
   g.checksums.assign(g.totalCells, 0u);
   g.changed.assign(g.totalCells, 0u);
   g.stats.sampledBytes = 0;

   const size_t cellBytes = g.cellBytes();
   const size_t CHUNK = 1u << 20; // 1 MB reads
   std::vector<uint8_t> buf(CHUNK);
   uint64_t budget = byteBudget;

   for (size_t ri = 0; ri < g.regions.size(); ++ri)
   {
      const auto& r = g.regions[ri];
      size_t start = g.regionCellStart[ri];
      size_t end = g.regionCellStart[ri + 1];

      if (r.state != memscan::RegionState::Committed)
      {
         for (size_t i = start; i < end; ++i)
         {
            g.pixels[i] = col::Reserved;
         }
         continue;
      }

      // Cache of the most recent chunk within this region.
      uintptr_t cacheBase = 0;
      size_t cacheLen = 0;

      for (size_t ci = start; ci < end; ++ci)
      {
         size_t off = (ci - start) * cellBytes;
         uintptr_t addr = r.base + off;
         size_t want = std::min(cellBytes, r.size - off);

         if (budget == 0)
         {
            g.pixels[ci] = col::Unsampled;
            continue;
         }

         // Ensure [addr, addr+want) is covered by the cache.
         if (!(addr >= cacheBase && addr + want <= cacheBase + cacheLen))
         {
            size_t readWant = (size_t)std::min<uint64_t>(
               CHUNK, std::min<uint64_t>(budget, r.size - off));
            size_t got = memscan::readMemory(h, addr, buf.data(), readWant);
            cacheBase = addr;
            cacheLen = got;
            budget -= std::min<uint64_t>(readWant, budget);
            g.stats.sampledBytes += got;
         }

         size_t avail = 0;
         if (addr >= cacheBase && addr < cacheBase + cacheLen)
         {
            avail = std::min(want, (size_t)(cacheBase + cacheLen - addr));
         }

         if (avail == 0)
         {
            g.pixels[ci] = col::Unreadable;
            g.checksums[ci] = 0;
            continue;
         }

         const uint8_t* p = buf.data() + (addr - cacheBase);
         BlockMetrics m = analyzeBytes(p, avail);
         g.checksums[ci] = checksum(p, avail);
         if (m.zeroRatio > 0.995)
         {
            g.pixels[ci] = col::Zeroed;
         }
         else
         {
            g.pixels[ci] = entropyColor(m.entropy);
         }
      }
   }
}

void markChanges(const Grid& prev, Grid& cur)
{
   if (prev.totalCells != cur.totalCells)
   {
      return;
   }
   if (prev.regions.size() != cur.regions.size())
   {
      return;
   }
   if (prev.checksums.size() != cur.checksums.size())
   {
      return;
   }
   // Layout compatibility: same region bases/sizes.
   for (size_t i = 0; i < cur.regions.size(); ++i)
   {
      if (prev.regions[i].base != cur.regions[i].base ||
          prev.regions[i].size != cur.regions[i].size)
      {
         return;
      }
   }
   size_t changed = 0;
   const uint32_t hi = rgba(255, 244, 120);
   for (size_t i = 0; i < cur.totalCells; ++i)
   {
      uint32_t a = prev.checksums[i];
      uint32_t b = cur.checksums[i];
      if (a != 0 && b != 0 && a != b)
      {
         cur.changed[i] = 1;
         cur.pixels[i] = hi;
         ++changed;
      }
   }
   cur.stats.changedCells = changed;
}

size_t cellIndex(const Grid& g, int x, int y)
{
   if (x < 0 || y < 0 || x >= g.width || y >= g.height)
   {
      return SIZE_MAX;
   }
   size_t idx = (size_t)y * g.width + x;
   if (idx >= g.totalCells)
   {
      return SIZE_MAX;
   }
   return idx;
}

bool resolveCell(const Grid& g, size_t index, size_t& regionIndex, uintptr_t& addr)
{
   if (index >= g.totalCells || g.regions.empty())
   {
      return false;
   }
   // Largest region start <= index.
   auto it = std::upper_bound(g.regionCellStart.begin(), g.regionCellStart.end(), index);
   if (it == g.regionCellStart.begin())
   {
      return false;
   }
   size_t ri = (size_t)(it - g.regionCellStart.begin()) - 1;
   if (ri >= g.regions.size())
   {
      return false;
   }
   regionIndex = ri;
   size_t offCells = index - g.regionCellStart[ri];
   addr = g.regions[ri].base + offCells * g.cellBytes();
   return true;
}

int maxCellPixels(int gridW, int gridH)
{
   int dim = std::max(gridW, gridH);
   if (dim <= 0)
   {
      return 1;
   }
   return MAX_TEX_DIM / dim;
}

void upscalePixels(const std::vector<uint32_t>& src, int srcW, int srcH, int cellPx,
                   std::vector<uint32_t>& dst, int& dstW, int& dstH)
{
   if (cellPx < 1)
   {
      cellPx = 1;
   }
   dstW = srcW * cellPx;
   dstH = srcH * cellPx;
   dst.assign((size_t)dstW * dstH, rgba(18, 18, 22));
   if (srcW <= 0 || srcH <= 0 || src.empty())
   {
      return;
   }

   for (int cy = 0; cy < srcH; ++cy)
   {
      for (int cx = 0; cx < srcW; ++cx)
      {
         size_t srcIdx = (size_t)cy * srcW + cx;
         if (srcIdx >= src.size())
         {
            return;
         }
         uint32_t c = src[srcIdx];
         int dy0 = cy * cellPx;
         int dx0 = cx * cellPx;
         for (int py = 0; py < cellPx; ++py)
         {
            uint32_t* row = dst.data() + (size_t)(dy0 + py) * dstW + dx0;
            for (int px = 0; px < cellPx; ++px)
            {
               row[px] = c;
            }
         }
      }
   }
}

} // namespace model
