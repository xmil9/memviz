#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "memscan.h"

// Turns the raw region list into a packed 2D grid of fixed-size blocks and a
// GPU-ready RGBA pixel buffer. Free regions are skipped; reserved + committed
// regions are laid out end-to-end and wrapped to `width` cells per row.
namespace model
{

constexpr size_t BLOCK_SIZE = 1024; // 1 KB per block (at 1x aggregation)
constexpr int MAX_TEX_DIM = 16384;  // D3D11 max 2D texture dimension

enum class ColorMode
{
   Metadata,
   Content
};

// Pack to R8G8B8A8 (bytes in memory: R,G,B,A) to match DXGI_FORMAT_R8G8B8A8_UNORM.
inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
   return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

struct LegendItem
{
   const char* label;
   uint32_t color;
};

struct Stats
{
   uint64_t committedBytes = 0;
   uint64_t reservedBytes = 0;
   size_t regionCount = 0;    // committed + reserved
   uint64_t sampledBytes = 0; // bytes actually read in content mode
   size_t changedCells = 0;
};

struct Grid
{
   int width = 0;
   int height = 0;
   size_t blocksPerCell = 1; // KB represented by one cell
   size_t totalCells = 0;
   ColorMode mode = ColorMode::Metadata;

   std::vector<memscan::Region> regions; // packed order (address ascending)
   std::vector<size_t> regionCellStart;  // prefix sums, size regions+1

   std::vector<uint32_t> pixels;    // width*height RGBA
   std::vector<uint32_t> checksums; // per-cell (content mode; 0 = unreadable/none)
   std::vector<uint8_t> changed;    // per-cell flag vs previous grid

   Stats stats;

   size_t cellBytes() const { return BLOCK_SIZE * blocksPerCell; }
};

// Build grid layout from regions and fill metadata colors. `width` is desired
// cells per row; aggregation (blocksPerCell) is raised automatically if the grid
// would exceed the max texture dimension.
std::shared_ptr<Grid> buildGrid(const std::vector<memscan::Region>& regions, int width);

// Recolor the grid by reading actual bytes (committed regions only), bounded by
// byteBudget total bytes read. Fills checksums for change detection.
void fillContent(HANDLE h, Grid& g, uint64_t byteBudget);

// Compare checksums with a previous grid of identical layout; tag + tint changed
// cells. No-op if layouts differ.
void markChanges(const Grid& prev, Grid& cur);

// Map a cell (x,y) to a linear index; returns SIZE_MAX if outside the used area.
size_t cellIndex(const Grid& g, int x, int y);

// Resolve a linear cell index to its region index and start address.
// Returns false if index is out of range.
bool resolveCell(const Grid& g, size_t index, size_t& regionIndex, uintptr_t& addr);

// Color legend matching the metadata palette.
std::vector<LegendItem> metadataLegend();
std::vector<LegendItem> contentLegend();

// Per-block metrics for a single chunk of bytes (used for the detail panel).
struct BlockMetrics
{
   double entropy = 0.0;   // normalized [0,1]
   double zeroRatio = 0.0; // [0,1]
   bool readable = false;
};
BlockMetrics analyzeBytes(const uint8_t* data, size_t n);

// Max pixels-per-cell upscale that keeps the texture within MAX_TEX_DIM.
int maxCellPixels(int gridW, int gridH);

// Expand each source cell to cellPx x cellPx blocks for crisp zoomed display.
void upscalePixels(const std::vector<uint32_t>& src, int srcW, int srcH, int cellPx,
                   std::vector<uint32_t>& dst, int& dstW, int& dstH);

} // namespace model
