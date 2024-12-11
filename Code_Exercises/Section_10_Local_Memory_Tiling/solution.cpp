/*
 SYCL Academy (c)

 SYCL Academy is licensed under a Creative Commons
 Attribution-ShareAlike 4.0 International License.

 You should have received a copy of the license along with this
 work.  If not, see <http://creativecommons.org/licenses/by-sa/4.0/>.
*/

#include "../helpers.hpp"

#include <algorithm>
#include <iostream>

#include <sycl/sycl.hpp>

#include <benchmark.h>
#include <image_conv.h>

inline constexpr util::filter_type filterType = util::filter_type::blur;
inline constexpr int filterWidth = 11;
inline constexpr int halo = filterWidth / 2;

int main() {
  const char* inputImageFile = "Code_Exercises/Images/dogs.png";
  const char* outputImageFile = "Code_Exercises/Images/blurred_dogs.png";

  auto inputImage = util::read_image(inputImageFile, halo);

  auto outputImage = util::allocate_image(
      inputImage.width(), inputImage.height(), inputImage.channels());

  auto filter = util::generate_filter(filterType, filterWidth);

  try {
    sycl::queue myQueue { sycl::gpu_selector_v };

    std::cout << "Running on "
              << myQueue.get_device().get_info<sycl::info::device::name>()
              << "\n";

    auto inputImgWidth = inputImage.width();
    auto inputImgHeight = inputImage.height();
    auto channels = inputImage.channels();
    auto filterWidth = filter.width();
    auto halo = filter.half_width();

    auto globalRange = sycl::range(inputImgHeight, inputImgWidth);
    auto localRange = sycl::range(8, 8);
    auto ndRange = sycl::nd_range(globalRange, localRange);

    auto inBufRange =
        sycl::range(inputImgHeight + (halo * 2), inputImgWidth + (halo * 2));
    auto outBufRange = sycl::range(inputImgHeight, inputImgWidth);
    auto filterRange = sycl::range(filterWidth, filterWidth);
    auto scratchpadRange = localRange + sycl::range(halo * 2, halo * 2);

    auto inDev =
        sycl::malloc_device<float>(inBufRange.size() * channels, myQueue);
    auto outDev =
        sycl::malloc_device<float>(outBufRange.size() * channels, myQueue);
    auto filterDev =
        sycl::malloc_device<float>(filterRange.size() * channels, myQueue);

    myQueue.copy<float>(inputImage.data(), inDev, inBufRange.size() * channels);
    myQueue.copy<float>(filter.data(), filterDev,
                        filterRange.size() * channels);

    auto inDev4 = reinterpret_cast<sycl::float4*>(inDev);
    auto filterDev4 = reinterpret_cast<sycl::float4*>(filterDev);
    auto outDev4 = reinterpret_cast<sycl::float4*>(outDev);

    // synchronize before benchmark, to not measure data transfers.
    myQueue.wait_and_throw();

    util::benchmark(
        [&] {
          myQueue.submit([&](sycl::handler& cgh) {
            auto scratchpad =
                sycl::local_accessor<sycl::float4, 2>(scratchpadRange, cgh);

            cgh.parallel_for(ndRange, [=](sycl::nd_item<2> item) {
              auto globalId = item.get_global_id();
              auto groupId = item.get_group().get_group_id();
              auto localId = item.get_local_id();
              auto globalGroupOffset = groupId * localRange;

              /*
               * Each work group will need to read a tile of size
               * (localRange[0] + halo * 2, localRange[1] + halo * 2) in
               * order to write a tile of size (localRange[0],
               * localRange[1]). Since the size of the tile we need to
               * read is larger than the workgroup size (localRange), we
               * must do multiple loads per work item. The iterations of
               * the for loop work are as follows:
               *
               *            <- localRange[0] + halo *2 ->
               *           +------------------------------+  ^
               *           |+-----------------++---------+|  |
               *         ^ ||<-localRange[0]->||         ||  |
               *         | ||                 ||         ||  |
               *     local ||   iteration 1   ||  it 2   ||  |
               *  Range[1] ||     load        ||  load   ||
               *         | ||                 ||         ||  localRange[1]
               * + | ||                 ||         ||  halo * 2 V || || ||
               *           |+-----------------++---------+|  |
               *           |+-----------------++---------+|  |
               *           ||                 ||         ||  |
               *           ||    it  3 load   ||it 4 load||  |
               *           ||                 ||         ||  |
               *           |+-----------------++---------+|  |
               *           +------------------------------+  V
               */

              for (auto i = localId[0]; i < scratchpadRange[0];
                   i += localRange[0]) {
                for (auto j = localId[1]; j < scratchpadRange[1];
                     j += localRange[1]) {
                  scratchpad[i][j] =
                      inDev4[(globalGroupOffset[0] + i) * inBufRange[1] +
                             globalGroupOffset[1] + j];
                }
              }

              sycl::group_barrier(item.get_group());

              auto sum = sycl::float4 { 0.0f, 0.0f, 0.0f, 0.0f };

              for (int r = 0; r < filterWidth; ++r) {
                for (int c = 0; c < filterWidth; ++c) {
                  auto idx = sycl::range(r, c);
                  sum += scratchpad[localId + idx] *
                         filterDev4[idx[0] * filterRange[1] + idx[1]];
                }
              }

              outDev4[globalId[0] * outBufRange[1] + globalId[1]] = sum;
            });
          });

          myQueue.wait_and_throw();
        },
        100, "image convolution (tiled)");
    myQueue.copy<float>(outDev, outputImage.data(),
                        outBufRange.size() * channels).wait();
    sycl::free(inDev, myQueue);
    sycl::free(outDev, myQueue);
    sycl::free(filterDev, myQueue);
  } catch (const sycl::exception& e) {
    std::cout << "Exception caught: " << e.what() << std::endl;
  }

  util::write_image(outputImage, outputImageFile);

  SYCLACADEMY_ASSERT(true);
}
