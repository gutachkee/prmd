// Multithreaded encoding (EncodeOptions::threads, design.md §1.4):
//   * golden hashes pin the serial encoder's output to that of commit fc0a629
//     (before threading) for every case of tests/thread_matrix.h;
//   * every thread count (2, 3, 7, 16, 0 = hardware, more threads than
//     blocks) writes the same bytes as threads = 1;
//   * thread-count resolution, EncodeReport::threads, option validation;
//   * row-parallel transforms equal their serial versions;
//   * multithreaded files decode exactly; concurrent encodes are independent;
//   * failures: the same exception at every thread count, no output file;
//   * J2K output is identical across thread counts.

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/parallel.h"
#include "prmd/internal/planes.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/internal/transform.h"
#include "reference.h"
#include "test_util.h"
#include "thread_matrix.h"

using namespace prmd;
using threadmatrix::Case;

namespace {

// FNV-1a 64 of each case's file, written by the serial encoder of commit
// fc0a629 (scratch program linking that commit's prmd_core and
// tests/thread_matrix.h). Regenerate only for an intended format change.
const std::map<std::string, uint64_t>& golden() {
  static const std::map<std::string, uint64_t> g = {
    {"haar_1x1_gray", 0x20037edf48341856ull},
    {"haar_1x1_rgb", 0x6b0262600b125d7bull},
    {"haar_3x2_gray", 0xb493ed088e7fbf0aull},
    {"haar_3x2_rgb", 0xd2ad71050521eabcull},
    {"haar_17x1_gray", 0xe307940f5d2156e8ull},
    {"haar_17x1_rgb", 0x20b198c394ed0961ull},
    {"haar_1x23_gray", 0xc708c802ea1b8942ull},
    {"haar_1x23_rgb", 0x8d5ad22e5d4168d1ull},
    {"haar_100x100_gray", 0xdbc4c9b0b4613508ull},
    {"haar_100x100_rgb", 0x7a60c953cab4ac5cull},
    {"haar_300x200_gray", 0x33472c24c4d25be8ull},
    {"haar_300x200_rgb", 0x3634ddbe1d56ecf4ull},
    {"haar_257x129_gray", 0xe5de8d0c10f76906ull},
    {"haar_257x129_rgb", 0x1e087e4a2a858fc5ull},
    {"haar_1000x777_gray", 0x77fafbaa6ff7c067ull},
    {"haar_1000x777_rgb", 0xbf801ca41ecbbf84ull},
    {"cdf53_1x1_gray", 0x1e0ffd29a46e993bull},
    {"cdf53_1x1_rgb", 0x4893d97220b82d2eull},
    {"cdf53_3x2_gray", 0x69ef643d16b3f4acull},
    {"cdf53_3x2_rgb", 0x548ebe1b8b47356eull},
    {"cdf53_17x1_gray", 0x898712433ac9b2ccull},
    {"cdf53_17x1_rgb", 0xc3c269abfc38442dull},
    {"cdf53_1x23_gray", 0xd8d89873d8b1f31bull},
    {"cdf53_1x23_rgb", 0xb8a9d7e45312ccc4ull},
    {"cdf53_100x100_gray", 0x2a9fffaaad3916f6ull},
    {"cdf53_100x100_rgb", 0x9b7332eaa6dd30feull},
    {"cdf53_300x200_gray", 0xfc5fef69a123fb73ull},
    {"cdf53_300x200_rgb", 0x00d1d1d0b3120443ull},
    {"cdf53_257x129_gray", 0xd26205d5b09e5a5bull},
    {"cdf53_257x129_rgb", 0x05f45194ef1a469bull},
    {"cdf53_1000x777_gray", 0xd71396eeecd5757dull},
    {"cdf53_1000x777_rgb", 0x9204ed9ff07292baull},
    {"lapbox_1x1_gray", 0x4273b8da86738220ull},
    {"lapbox_1x1_rgb", 0x8f91eb36b8c06e58ull},
    {"lapbox_3x2_gray", 0xdbfb2583133f7e59ull},
    {"lapbox_3x2_rgb", 0x48ded6a8746ea1c4ull},
    {"lapbox_17x1_gray", 0x64df097ab971b5d3ull},
    {"lapbox_17x1_rgb", 0x2c19f0b997536198ull},
    {"lapbox_1x23_gray", 0xc4d3c994b10ee8b7ull},
    {"lapbox_1x23_rgb", 0x800140598bd74a1eull},
    {"lapbox_100x100_gray", 0x46a25fbd49c2caa3ull},
    {"lapbox_100x100_rgb", 0xcb0d7675bc5fff98ull},
    {"lapbox_300x200_gray", 0x9577963fd8d4016full},
    {"lapbox_300x200_rgb", 0xfb5b9402c0725ea8ull},
    {"lapbox_257x129_gray", 0xa38514cabd3c4e68ull},
    {"lapbox_257x129_rgb", 0x5a3cc0dfd68d0104ull},
    {"lapbox_1000x777_gray", 0x87b2e5f8e17a28b8ull},
    {"lapbox_1000x777_rgb", 0x155e1a2e1cdd92c5ull},
    {"lapgauss_1x1_gray", 0x3dd77fa8ba5d216bull},
    {"lapgauss_1x1_rgb", 0xdf83f4558f3a3b33ull},
    {"lapgauss_3x2_gray", 0x5b175030a071432bull},
    {"lapgauss_3x2_rgb", 0xd9e99fc9a6b4be78ull},
    {"lapgauss_17x1_gray", 0xa25c9c4fdb7563b7ull},
    {"lapgauss_17x1_rgb", 0x3503526c24deac15ull},
    {"lapgauss_1x23_gray", 0xbff604fb2f4c58b5ull},
    {"lapgauss_1x23_rgb", 0xc406f8816b3edf63ull},
    {"lapgauss_100x100_gray", 0xbd6380dcea5ba95cull},
    {"lapgauss_100x100_rgb", 0x74104f134ff6b11aull},
    {"lapgauss_300x200_gray", 0xda881fddf7dd89ecull},
    {"lapgauss_300x200_rgb", 0x38a677902e81693full},
    {"lapgauss_257x129_gray", 0x8fabd05d6072f941ull},
    {"lapgauss_257x129_rgb", 0xea9906512de71165ull},
    {"lapgauss_1000x777_gray", 0x14eba2975fe47186ull},
    {"lapgauss_1000x777_rgb", 0xafafbda696ce1664ull},
    {"naive_1x1_gray", 0x55134e075adbaa97ull},
    {"naive_1x1_rgb", 0x22dc2a75e173137aull},
    {"naive_3x2_gray", 0x2c6e9f5f3913deafull},
    {"naive_3x2_rgb", 0x0bd682b2afebe5bbull},
    {"naive_17x1_gray", 0xd14014bd0f9d3eb2ull},
    {"naive_17x1_rgb", 0x00c9cb9751687ab8ull},
    {"naive_1x23_gray", 0xca48d55e5a48ebf8ull},
    {"naive_1x23_rgb", 0x7b2f3a71d60e1e4aull},
    {"naive_100x100_gray", 0x9064d8a9331e2f2bull},
    {"naive_100x100_rgb", 0x70f4ea1552c743d1ull},
    {"naive_300x200_gray", 0xd9e2b3c3fe54748eull},
    {"naive_300x200_rgb", 0x2e2ee0e4e053d8fdull},
    {"naive_257x129_gray", 0x5b408a91adc108c2ull},
    {"naive_257x129_rgb", 0x57e2d996e6d84525ull},
    {"naive_1000x777_gray", 0xb3c2a897796abc22ull},
    {"naive_1000x777_rgb", 0xff1fe2c248a6af10ull},
    {"naivegauss_1x1_gray", 0x8c3a2351dc0cf548ull},
    {"naivegauss_1x1_rgb", 0x3d3c31aefdb8a745ull},
    {"naivegauss_3x2_gray", 0x74870e16a4a3e3c0ull},
    {"naivegauss_3x2_rgb", 0x39c70ba09315a4b7ull},
    {"naivegauss_17x1_gray", 0xb57bf778713af524ull},
    {"naivegauss_17x1_rgb", 0x1e9032a669b5fb93ull},
    {"naivegauss_1x23_gray", 0xa13202580d332a26ull},
    {"naivegauss_1x23_rgb", 0xfd99bdc7bda68300ull},
    {"naivegauss_100x100_gray", 0x0334d50f46e530fcull},
    {"naivegauss_100x100_rgb", 0xefa0fd126af8b472ull},
    {"naivegauss_300x200_gray", 0x1a4ab14db2af3d8eull},
    {"naivegauss_300x200_rgb", 0xadd947ddf43c6e7aull},
    {"naivegauss_257x129_gray", 0xfe7b415154fa137bull},
    {"naivegauss_257x129_rgb", 0x243eb89febfa3ed6ull},
    {"naivegauss_1000x777_gray", 0xccce8650ad866e06ull},
    {"naivegauss_1000x777_rgb", 0x00a56e8924aa8ad2ull},
    {"haar_range_1x1_gray", 0xb6284c079cd20c1eull},
    {"haar_range_1x1_rgb", 0xa1ef41a1cf0fa896ull},
    {"haar_range_3x2_gray", 0x8ea79a7c9d43083eull},
    {"haar_range_3x2_rgb", 0x104acd802aff2f00ull},
    {"haar_range_17x1_gray", 0xa9aaab0a9c1236caull},
    {"haar_range_17x1_rgb", 0xd08dfb87c0580bd7ull},
    {"haar_range_1x23_gray", 0xd7b098e82bbe4e77ull},
    {"haar_range_1x23_rgb", 0x00aff00f73042b32ull},
    {"haar_range_100x100_gray", 0x9429e015914181eaull},
    {"haar_range_100x100_rgb", 0xc6dabc525b4fc14eull},
    {"haar_range_300x200_gray", 0x11a6703a50116af9ull},
    {"haar_range_300x200_rgb", 0x8e7511a61e7259c8ull},
    {"haar_range_257x129_gray", 0x7f6ca5ec17d08aa8ull},
    {"haar_range_257x129_rgb", 0xb17cfc8974418323ull},
    {"haar_range_1000x777_gray", 0xcfd6ead4e5626fc1ull},
    {"haar_range_1000x777_rgb", 0x6f4550aba8186f74ull},
    {"cdf53_raw_1x1_gray", 0x2af7dbdddafc1724ull},
    {"cdf53_raw_1x1_rgb", 0x90c3cd059c68232eull},
    {"cdf53_raw_3x2_gray", 0x163e2e6a0131b9adull},
    {"cdf53_raw_3x2_rgb", 0xdb82afbe756f1a85ull},
    {"cdf53_raw_17x1_gray", 0x14280c7df84f7b5aull},
    {"cdf53_raw_17x1_rgb", 0xabfca0bfacecac26ull},
    {"cdf53_raw_1x23_gray", 0x323c4b28244478d9ull},
    {"cdf53_raw_1x23_rgb", 0xb5150d201294e358ull},
    {"cdf53_raw_100x100_gray", 0x64df93b223fd26c8ull},
    {"cdf53_raw_100x100_rgb", 0xc2fa9587777f027eull},
    {"cdf53_raw_300x200_gray", 0x82bd28869f2ce0d1ull},
    {"cdf53_raw_300x200_rgb", 0x3a31ae3447fad2e6ull},
    {"cdf53_raw_257x129_gray", 0x16c6ad1e5d400537ull},
    {"cdf53_raw_257x129_rgb", 0x04c53479b2a6952bull},
    {"cdf53_raw_1000x777_gray", 0x6f816b28e0bacecbull},
    {"cdf53_raw_1000x777_rgb", 0x1facc890fded13c6ull},
    {"naive_nomed_1x1_gray", 0x5bcffc32ef1e11eeull},
    {"naive_nomed_1x1_rgb", 0x9468d0b7dd4b455full},
    {"naive_nomed_3x2_gray", 0xec394518793f42b7ull},
    {"naive_nomed_3x2_rgb", 0x1378617636bad5a2ull},
    {"naive_nomed_17x1_gray", 0xcdd77be589d61afbull},
    {"naive_nomed_17x1_rgb", 0x2e59cfe7a25d6fd5ull},
    {"naive_nomed_1x23_gray", 0x050b05f5da88c1b3ull},
    {"naive_nomed_1x23_rgb", 0x61ed41c1a474dacfull},
    {"naive_nomed_100x100_gray", 0x64cd9b0720ac56b8ull},
    {"naive_nomed_100x100_rgb", 0x4335c30a00bfb57aull},
    {"naive_nomed_300x200_gray", 0xe484bf92b7a3fc86ull},
    {"naive_nomed_300x200_rgb", 0xdf76bf2cfc98edf2ull},
    {"naive_nomed_257x129_gray", 0xd67fe8c3d259dbefull},
    {"naive_nomed_257x129_rgb", 0xa2d5b066f06796caull},
    {"naive_nomed_1000x777_gray", 0x105a54c1e54d7998ull},
    {"naive_nomed_1000x777_rgb", 0xcfc0ea8fe9f68965ull},
    {"naive_nomed_noycocg_1x1_gray", 0x5bcffc32ef1e11eeull},
    {"naive_nomed_noycocg_1x1_rgb", 0x80d4f26c117a6023ull},
    {"naive_nomed_noycocg_3x2_gray", 0xec394518793f42b7ull},
    {"naive_nomed_noycocg_3x2_rgb", 0x09b2c539724827c6ull},
    {"naive_nomed_noycocg_17x1_gray", 0xcdd77be589d61afbull},
    {"naive_nomed_noycocg_17x1_rgb", 0x3a37d4d472a7e1c4ull},
    {"naive_nomed_noycocg_1x23_gray", 0x050b05f5da88c1b3ull},
    {"naive_nomed_noycocg_1x23_rgb", 0x761cba66b7bc0f9dull},
    {"naive_nomed_noycocg_100x100_gray", 0x64cd9b0720ac56b8ull},
    {"naive_nomed_noycocg_100x100_rgb", 0x1ba36f855481fc3dull},
    {"naive_nomed_noycocg_300x200_gray", 0xe484bf92b7a3fc86ull},
    {"naive_nomed_noycocg_300x200_rgb", 0x276d9bddfe91c65cull},
    {"naive_nomed_noycocg_257x129_gray", 0xd67fe8c3d259dbefull},
    {"naive_nomed_noycocg_257x129_rgb", 0xa916018fa4006c26ull},
    {"naive_nomed_noycocg_1000x777_gray", 0x105a54c1e54d7998ull},
    {"naive_nomed_noycocg_1000x777_rgb", 0x7ebb48783cdbb982ull},
    {"haar_nomed_noycocg_range_1x1_gray", 0x81dcd6c0ff30a43dull},
    {"haar_nomed_noycocg_range_1x1_rgb", 0xbb330f13897777ffull},
    {"haar_nomed_noycocg_range_3x2_gray", 0xe380be1fe04b82f1ull},
    {"haar_nomed_noycocg_range_3x2_rgb", 0xd5320b71890e313dull},
    {"haar_nomed_noycocg_range_17x1_gray", 0xec3e995e4d7399bbull},
    {"haar_nomed_noycocg_range_17x1_rgb", 0xdd9849641b9fcb8bull},
    {"haar_nomed_noycocg_range_1x23_gray", 0xbb58b10df8abf440ull},
    {"haar_nomed_noycocg_range_1x23_rgb", 0x22dd30a79c7fe95aull},
    {"haar_nomed_noycocg_range_100x100_gray", 0x1808131ebd41abecull},
    {"haar_nomed_noycocg_range_100x100_rgb", 0x1da1aafcf62ac731ull},
    {"haar_nomed_noycocg_range_300x200_gray", 0x9cd6c56c0f7151caull},
    {"haar_nomed_noycocg_range_300x200_rgb", 0xfbc24688b8e4beb0ull},
    {"haar_nomed_noycocg_range_257x129_gray", 0xa026ca9bbba57700ull},
    {"haar_nomed_noycocg_range_257x129_rgb", 0xc430c40f458750d7ull},
    {"haar_nomed_noycocg_range_1000x777_gray", 0x7af126fc9ae8b4b7ull},
    {"haar_nomed_noycocg_range_1000x777_rgb", 0x13c023b344d8ee03ull},
    {"lapbox_noycocg_1x1_gray", 0x4273b8da86738220ull},
    {"lapbox_noycocg_1x1_rgb", 0x0fd8daa8bfb3dee9ull},
    {"lapbox_noycocg_3x2_gray", 0xdbfb2583133f7e59ull},
    {"lapbox_noycocg_3x2_rgb", 0x5f2d9d46be08cca3ull},
    {"lapbox_noycocg_17x1_gray", 0x64df097ab971b5d3ull},
    {"lapbox_noycocg_17x1_rgb", 0xae8a5ab47771f013ull},
    {"lapbox_noycocg_1x23_gray", 0xc4d3c994b10ee8b7ull},
    {"lapbox_noycocg_1x23_rgb", 0x8e9882c50adfe32dull},
    {"lapbox_noycocg_100x100_gray", 0x46a25fbd49c2caa3ull},
    {"lapbox_noycocg_100x100_rgb", 0xb90aa5a255284fe0ull},
    {"lapbox_noycocg_300x200_gray", 0x9577963fd8d4016full},
    {"lapbox_noycocg_300x200_rgb", 0xb804705bf56bf531ull},
    {"lapbox_noycocg_257x129_gray", 0xa38514cabd3c4e68ull},
    {"lapbox_noycocg_257x129_rgb", 0x5c1ff2e9f72fcc40ull},
    {"lapbox_noycocg_1000x777_gray", 0x87b2e5f8e17a28b8ull},
    {"lapbox_noycocg_1000x777_rgb", 0xfd5344e17778ec40ull},
    {"haar_q1_gray", 0xd134142f8c46c947ull},
    {"haar_q1_rgb", 0xdb27720c4fb211f3ull},
    {"haar_q3_gray", 0x15cb9d472da7ff86ull},
    {"haar_q3_rgb", 0x92ee60262113b094ull},
    {"haar_q8_gray", 0x6e3879ec664a30f3ull},
    {"haar_q8_rgb", 0x3980ad8f326f0e7full},
    {"cdf53_q1_gray", 0xca343c3d6fc431efull},
    {"cdf53_q1_rgb", 0xb5cabe164fda612bull},
    {"cdf53_q3_gray", 0x8970e76be9a77ad1ull},
    {"cdf53_q3_rgb", 0x2348c7cca09a16a2ull},
    {"cdf53_q8_gray", 0xaedbd7ed3b36dbaaull},
    {"cdf53_q8_rgb", 0xde352b2ec33b8138ull},
    {"lapbox_q1_gray", 0x67d08790ff8bf5e6ull},
    {"lapbox_q1_rgb", 0x53e73f603e8bab19ull},
    {"lapbox_q3_gray", 0x09f3172f30f58965ull},
    {"lapbox_q3_rgb", 0xb31e7ecfbb47420aull},
    {"lapbox_q8_gray", 0xbf007a87fb28b9d7ull},
    {"lapbox_q8_rgb", 0x34c5ac1eb7c87119ull},
    {"lapgauss_q1_gray", 0xa5a9eeca32e6c364ull},
    {"lapgauss_q1_rgb", 0x8c892df953a078c1ull},
    {"lapgauss_q3_gray", 0x45b7656868d7e65full},
    {"lapgauss_q3_rgb", 0xd6a576428fcbdee7ull},
    {"lapgauss_q8_gray", 0x40e9d7fb3d154b97ull},
    {"lapgauss_q8_rgb", 0xb3039774bf779f06ull},
    {"haar_range_q1_gray", 0x9a4de298f18ac02aull},
    {"haar_range_q1_rgb", 0x1cf7f17454d75bb7ull},
    {"haar_range_q3_gray", 0x5c625ba10cde29e0ull},
    {"haar_range_q3_rgb", 0x6c6fa5f3d5362225ull},
    {"haar_range_q8_gray", 0x929d46b951a7e312ull},
    {"haar_range_q8_rgb", 0xba19194df03431ebull},
    {"haar_nomed_noycocg_range_q1_gray", 0x6ee423b4adf50bf5ull},
    {"haar_nomed_noycocg_range_q1_rgb", 0x8c807d208720cf8cull},
    {"haar_nomed_noycocg_range_q3_gray", 0x05745cd226babeb6ull},
    {"haar_nomed_noycocg_range_q3_rgb", 0x25f6e7969d474d40ull},
    {"haar_nomed_noycocg_range_q8_gray", 0x0d0afe030aa66fa1ull},
    {"haar_nomed_noycocg_range_q8_rgb", 0x79c096688cd4fd8eull},
    {"lapbox_noycocg_q1_gray", 0x67d08790ff8bf5e6ull},
    {"lapbox_noycocg_q1_rgb", 0xd28edc4a6ce5484cull},
    {"lapbox_noycocg_q3_gray", 0x09f3172f30f58965ull},
    {"lapbox_noycocg_q3_rgb", 0xab34d63b88ac6b51ull},
    {"lapbox_noycocg_q8_gray", 0xbf007a87fb28b9d7ull},
    {"lapbox_noycocg_q8_rgb", 0x7269677e78a56611ull},
    {"haar_zstd3_gray", 0x05d924e4babec93full},
    {"cdf53_q4_zstd3_gray", 0xcb458e8f24ca28a5ull},
    {"haar_zstd3_rgb", 0x0d79779ae9c4744dull},
    {"cdf53_q4_zstd3_rgb", 0xae28040e970334aeull},
    {"haar_zstd19_gray", 0x1517bbb3fffefcacull},
    {"cdf53_q4_zstd19_gray", 0x0468d58e8ed5d21eull},
    {"haar_zstd19_rgb", 0x968511b26d2df597ull},
    {"cdf53_q4_zstd19_rgb", 0xb69096471c1fa7d3ull},
    {"haar_zstd22_gray", 0xb3eae8091976a22cull},
    {"cdf53_q4_zstd22_gray", 0x1ee27d966adfcbb5ull},
    {"haar_zstd22_rgb", 0x8f711388406c3564ull},
    {"cdf53_q4_zstd22_rgb", 0x7b82fb01806f9731ull},
    {"haar_levels1", 0x2ac9772f67403307ull},
    {"haar_levels8_cblk16", 0xc64682c64ab96fa6ull},
    {"cdf53_levels8_cblk16", 0x079f8135e401a04cull},
    {"lapbox_q2_levels8_cblk16", 0x0f91f224518da19bull},
    {"haar_one_block_per_level", 0x418e36a4af481bc1ull},
    {"convert_rgb_to_gray", 0xb1e889f05c047c0dull},
    {"convert_gray_to_rgb_lap", 0x04516654eef1d570ull},
    {"haar_noise_rgb", 0xea68711f18e8d9f3ull},
    {"cdf53_noise_rgb", 0xeecbaa0a5763a335ull},
    {"lapbox_noise_rgb", 0x3500c0b7fa9b22e7ull},
    {"lapgauss_noise_rgb", 0xf8feefe6aa0622c8ull},
    {"naive_noise_rgb", 0xc086c8c798e3cd4aull},
    {"naivegauss_noise_rgb", 0xd3bf9e5682f854fcull},
    {"haar_range_noise_rgb", 0xed9312d0e6c5f976ull},
    {"cdf53_raw_noise_rgb", 0x5cbb825a585ef810ull},
    {"naive_nomed_noise_rgb", 0x18ba3b8c5ac74003ull},
    {"naive_nomed_noycocg_noise_rgb", 0x02e08dd9bf6dd558ull},
    {"haar_nomed_noycocg_range_noise_rgb", 0xc8b6dbcdf5289b64ull},
    {"lapbox_noycocg_noise_rgb", 0x91f64e9a0678cec1ull},
  };
  return g;
}

uint64_t file_hash(const std::string& path) {
  const std::vector<char> b = testutil::read_file(path);
  return threadmatrix::fnv1a64(b.data(), b.size());
}

const std::vector<Case>& all_cases() {
  static const std::vector<Case> c = threadmatrix::cases();
  return c;
}

const Image& source_of(const Case& c) {
  static std::map<std::string, Image> cache;
  auto it = cache.find(c.name);
  if (it == cache.end()) {
    it = cache.emplace(c.name, threadmatrix::source(c)).first;
  }
  return it->second;
}

struct Encoded {
  uint64_t hash = 0;
  EncodeReport report;
};

Encoded encode_case(const Case& c, int threads, const std::string& tag) {
  EncodeOptions o = c.opts;
  o.threads = threads;
  const std::string path =
      testutil::temp_path(c.name + "_" + tag + ".prmd").string();
  Encoded e;
  e.report = encode_file(source_of(c), path, Codec::Prmd, o);
  e.hash = file_hash(path);
  std::filesystem::remove(path);
  return e;
}

// Thread counts beyond 1: odd counts, more threads than this machine has, 0
// (hardware), and more threads than any case has blocks.
const int kThreadCounts[] = {2, 3, 7, 16, 0, 1000};

class ThreadMatrixTest : public ::testing::TestWithParam<size_t> {
 protected:
  const Case& the_case() const { return all_cases()[GetParam()]; }
};

std::string case_name(const ::testing::TestParamInfo<size_t>& info) {
  return all_cases()[info.param].name;
}

}  // namespace

// ----------------------------------------------------------------------------
// Golden output and determinism over the full matrix

TEST(ThreadMatrix, GoldenTableCoversEveryCase) {
  std::set<std::string> names;
  for (const Case& c : all_cases()) {
    EXPECT_TRUE(names.insert(c.name).second) << "duplicate " << c.name;
    EXPECT_EQ(golden().count(c.name), 1u) << c.name;
  }
  EXPECT_EQ(names.size(), golden().size());
}

TEST_P(ThreadMatrixTest, SerialEncoderMatchesGoldenHash) {
  const Case& c = the_case();
  const Encoded e = encode_case(c, 1, "serial");
  EXPECT_EQ(e.report.threads, 1);
  ASSERT_EQ(golden().count(c.name), 1u);
  EXPECT_EQ(e.hash, golden().at(c.name))
      << c.name << ": the serial encoder's output changed";
}

TEST_P(ThreadMatrixTest, EveryThreadCountWritesTheSameFile) {
  const Case& c = the_case();
  const uint64_t expected = golden().at(c.name);
  for (const int n : kThreadCounts) {
    const Encoded e = encode_case(c, n, "t" + std::to_string(n));
    EXPECT_EQ(e.hash, expected) << c.name << " threads=" << n;
    EXPECT_GE(e.report.threads, 1);
    EXPECT_LE(e.report.threads, internal::kMaxThreads);
  }
}

INSTANTIATE_TEST_SUITE_P(All,
                         ThreadMatrixTest,
                         ::testing::Range(size_t(0), all_cases().size()),
                         case_name);

// ----------------------------------------------------------------------------
// Thread count: resolution, clamping, report, validation

namespace {

Case case_named(const std::string& name) {
  for (const Case& c : all_cases()) {
    if (c.name == name) {
      return c;
    }
  }
  throw std::logic_error("no case " + name);
}

int present_blocks(const Case& c) {
  const auto geom = internal::make_geometry(
      c.w, c.h, c.opts.levels, c.opts.tile, c.opts.cblk, c.opts.transform);
  int n = 0;
  for (const auto& g : geom) {
    n += g.blocks_x * g.blocks_y;
  }
  return n;
}

}  // namespace

TEST(Threads, ReportCountsTheThreadsUsed) {
  // 300×200 at cblk 32: 94 blocks over 5 levels.
  const Case c = case_named("haar_300x200_gray");
  const int blocks = present_blocks(c);
  ASSERT_GT(blocks, 20);
  EXPECT_EQ(encode_case(c, 1, "r1").report.threads, 1);
  EXPECT_EQ(encode_case(c, 2, "r2").report.threads, 2);
  EXPECT_EQ(encode_case(c, 7, "r7").report.threads, 7);
  EXPECT_EQ(encode_case(c, 0, "r0").report.threads,
            std::min(internal::resolve_threads(0), blocks));
  // Clamped to the block count, which is below kMaxThreads here.
  EXPECT_EQ(encode_case(c, 1000, "r1000").report.threads, blocks);
  EXPECT_EQ(encode_case(c, INT_MAX, "rmax").report.threads, blocks);
}

TEST(Threads, NeverMoreThreadsThanBlocks) {
  // One block per level: 1×1 with 6 levels has 6 blocks.
  const Case tiny = case_named("haar_1x1_gray");
  EXPECT_EQ(present_blocks(tiny), 6);
  EXPECT_EQ(encode_case(tiny, 16, "tiny16").report.threads, 6);
  // A single block in the whole file: no pool at all.
  const Case one = case_named("haar_levels1");
  Case single = one;
  single.opts.cblk = 512;
  single.opts.tile = 512;
  EXPECT_EQ(present_blocks(single), 1);
  EXPECT_EQ(encode_case(single, 16, "single16").report.threads, 1);
  EXPECT_EQ(encode_case(single, 0, "single0").report.threads, 1);
}

TEST(Threads, ManyThreadsAreClampedTo256) {
  // cblk 16 over 1000×777 gray: thousands of blocks, so only the 256 cap
  // applies.
  Case c = case_named("haar_1000x777_gray");
  c.opts.cblk = 16;
  c.opts.tile = 16;
  ASSERT_GT(present_blocks(c), 256);
  EXPECT_EQ(encode_case(c, 300, "t300").report.threads, internal::kMaxThreads);
  EXPECT_EQ(encode_case(c, 300, "t300b").hash, encode_case(c, 1, "t1").hash);
}

TEST(Threads, NegativeIsRejectedBeforeAnyFileIsWritten) {
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  for (const Codec codec : {Codec::Prmd, Codec::J2K}) {
    for (const int bad : {-1, -8, INT_MIN}) {
      EncodeOptions o;
      o.levels = 3;
      o.threads = bad;
      const std::string path = testutil::temp_path("neg.out").string();
      std::filesystem::remove(path);
      try {
        encode_file(img, path, codec, o);
        ADD_FAILURE() << "threads " << bad << " accepted";
      } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("threads"), std::string::npos)
            << e.what();
      }
      EXPECT_FALSE(std::filesystem::exists(path));
    }
  }
  EncodeOptions o;
  o.threads = -1;
  EXPECT_THROW(internal::validate_prmd_options(o), std::invalid_argument);
  o.threads = 0;
  EXPECT_NO_THROW(internal::validate_prmd_options(o));
  o.threads = INT_MAX;
  EXPECT_NO_THROW(internal::validate_prmd_options(o));
}

TEST(Threads, DefaultIsHardwareConcurrency) {
  EXPECT_EQ(EncodeOptions{}.threads, 0);
  EXPECT_EQ(EncodeReport{}.threads, 1);
}

// ----------------------------------------------------------------------------
// Row-parallel building blocks equal their serial versions

namespace {

internal::Region random_plane(int w, int h, unsigned seed, int lo, int hi) {
  internal::Region r(Rect{0, 0, w, h});
  uint32_t s = seed * 747796405u + 1;
  for (auto& v : r.v) {
    s = s * 1664525u + 1013904223u;
    v = lo + int((s >> 8) % uint32_t(hi - lo + 1));
  }
  return r;
}

const std::pair<int, int> kPlaneSizes[] = {
    {1, 1},   {2, 3},    {3, 2},     {17, 1},   {1, 17},
    {129, 257}, {1000, 37}, {37, 1000}, {513, 300}};

}  // namespace

TEST(ParallelTransforms, ForwardEqualsSerial) {
  using internal::Region;
  for (const Transform t :
       {Transform::Haar, Transform::Cdf53, Transform::Laplacian}) {
    for (const Downsampler d : {Downsampler::Box, Downsampler::Gauss}) {
      const auto xf = internal::make_transform(t, d);
      for (const auto& [w, h] : kPlaneSizes) {
        // Unclamped inputs, as Cdf53 levels below 0 are.
        const Region fine = random_plane(w, h, unsigned(w * 7 + h), -300, 600);
        Region c1;
        std::vector<Region> b1;
        xf->forward(fine, c1, b1, nullptr);
        for (const int n : {2, 3, 7}) {
          internal::Pool pool(n);
          Region cn;
          std::vector<Region> bn;
          xf->forward(fine, cn, bn, &pool);
          ASSERT_EQ(cn.rect, c1.rect);
          ASSERT_EQ(cn.v, c1.v) << to_string(t) << " " << w << "x" << h;
          ASSERT_EQ(bn.size(), b1.size());
          for (size_t b = 0; b < b1.size(); ++b) {
            ASSERT_EQ(bn[b].v, b1[b].v)
                << to_string(t) << " band " << b << " " << w << "x" << h;
          }
        }
      }
    }
  }
}

TEST(ParallelTransforms, DownsampleAndUpsampleEqualSerial) {
  using internal::Region;
  for (const Downsampler d : {Downsampler::Box, Downsampler::Gauss}) {
    const auto lap = internal::make_transform(Transform::Laplacian, d);
    for (const auto& [w, h] : kPlaneSizes) {
      const Region fine = random_plane(w, h, unsigned(w + 31 * h), 0, 255);
      const Region c1 = internal::downsample(fine, d, nullptr);
      // The prediction U(coarse) is the inverse with a zero residual.
      const internal::Dims fd{w, h};
      Region u1;
      internal::laplacian_upsample(d, c1, fd, u1, nullptr);
      Region via_inverse;
      lap->inverse(c1, {Region(Rect{0, 0, w, h})}, Rect{0, 0, w, h}, fd,
                   via_inverse);
      ASSERT_EQ(u1.v, via_inverse.v) << w << "x" << h;
      for (const int n : {2, 3, 7}) {
        internal::Pool pool(n);
        ASSERT_EQ(internal::downsample(fine, d, &pool).v, c1.v)
            << to_string(d) << " " << w << "x" << h;
        Region un;
        internal::laplacian_upsample(d, c1, fd, un, &pool);
        ASSERT_EQ(un.v, u1.v) << to_string(d) << " " << w << "x" << h;
      }
    }
  }
}

TEST(ParallelTransforms, PlanesFromImageEqualConvertThenSplit) {
  for (const auto& [w, h] : kPlaneSizes) {
    for (const ColorFormat from : {ColorFormat::Gray8, ColorFormat::RGB8}) {
      const Image img = threadmatrix::pattern_image(w, h, from, 5, true);
      for (const ColorFormat to : {ColorFormat::Gray8, ColorFormat::RGB8}) {
        const auto expected = internal::to_planes(convert(img, to));
        for (const int n : {1, 2, 7}) {
          internal::Pool pool(n);
          const auto got = internal::to_planes(img, to, &pool);
          ASSERT_EQ(got.size(), expected.size());
          for (size_t c = 0; c < got.size(); ++c) {
            ASSERT_EQ(got[c].v, expected[c].v) << w << "x" << h;
          }
        }
        const auto serial = internal::to_planes(img, to, nullptr);
        for (size_t c = 0; c < serial.size(); ++c) {
          ASSERT_EQ(serial[c].v, expected[c].v);
        }
      }
    }
  }
}

namespace {

std::mutex g_stripe_m;
std::set<std::thread::id> g_stripe_threads;

void record_stripe(int, int) {
  const std::lock_guard<std::mutex> lock(g_stripe_m);
  g_stripe_threads.insert(std::this_thread::get_id());
}

struct StripeObserver {
  StripeObserver() {
    const std::lock_guard<std::mutex> lock(g_stripe_m);
    g_stripe_threads.clear();
    internal::test_hooks::set_row_stripe_observer(record_stripe);
  }
  ~StripeObserver() {
    internal::test_hooks::set_row_stripe_observer(nullptr);
  }
  std::set<std::thread::id> threads() const {
    const std::lock_guard<std::mutex> lock(g_stripe_m);
    return g_stripe_threads;
  }
};

}  // namespace

TEST(ParallelTransforms, RowStripesRunOnWorkerThreads) {
  using internal::Region;
  // 2048² samples: hundreds of stripes per pass, enough for every worker.
  const Region fine = random_plane(2048, 2048, 5, 0, 255);
  for (const Transform t : {Transform::Haar, Transform::Cdf53}) {
    const auto xf = internal::make_transform(t, Downsampler::Box);
    internal::Pool pool(4);
    ASSERT_EQ(pool.size(), 4);
    const StripeObserver observer;
    Region c;
    std::vector<Region> b;
    xf->forward(fine, c, b, &pool);
    const auto ids = observer.threads();
    EXPECT_GT(ids.size(), 1u) << to_string(t);
    EXPECT_LE(ids.size(), 4u) << to_string(t);
  }
  {
    internal::Pool pool(4);
    const StripeObserver observer;
    (void)internal::downsample(fine, Downsampler::Gauss, &pool);
    EXPECT_GT(observer.threads().size(), 1u) << "gauss downsample";
  }
  // Serial: one stripe, on this thread.
  const StripeObserver observer;
  Region c;
  std::vector<Region> b;
  internal::make_transform(Transform::Haar, Downsampler::Box)
      ->forward(fine, c, b, nullptr);
  EXPECT_EQ(observer.threads(),
            std::set<std::thread::id>{std::this_thread::get_id()});
}

// ----------------------------------------------------------------------------
// Multithreaded files decode exactly

TEST(ThreadsRoundTrip, LosslessMultithreadedFilesDecodeToTheReference) {
  for (const auto& cfg : testutil::prmd_configs()) {
    for (const ColorFormat f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
      const Image src = ref::synthetic_image(700, 413, f, 21);
      const auto expected = ref::pyramid(src, 5, cfg.t, cfg.d);
      EncodeOptions o = testutil::options_for(cfg, 5, f, 128, 64);
      o.threads = 5;
      const std::string path =
          testutil::temp_path(std::string("rt_") + cfg.name + ".prmd").string();
      const EncodeReport rep = encode_file(src, path, Codec::Prmd, o);
      EXPECT_EQ(rep.threads, 5);
      auto dec = open_decoder(path);
      for (int L = 0; L < 5; ++L) {
        const LevelInfo li = dec->level_info(L);
        const DecodeResult r = dec->decode(L, {0, 0, li.width, li.height});
        ASSERT_EQ(r.image.pixels, expected[size_t(L)].pixels)
            << cfg.name << " " << info(f).name << " L" << L;
        // A crop across block and tile borders.
        const Rect c = intersect({li.width / 3, li.height / 4, 150, 90},
                                 {0, 0, li.width, li.height});
        ASSERT_EQ(dec->decode(L, c).image.pixels,
                  expected[size_t(L)].crop(c).pixels);
      }
    }
  }
}

TEST(ThreadsRoundTrip, LossyMultithreadedFilesDecodeLikeSerialOnes) {
  for (const Transform t :
       {Transform::Haar, Transform::Cdf53, Transform::Laplacian}) {
    for (const ColorFormat f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
      const Image src = ref::synthetic_image(640, 360, f, 8);
      EncodeOptions o;
      o.transform = t;
      o.color = f;
      o.quant = 6;
      o.tile = 128;
      o.cblk = 64;
      o.threads = 1;
      const std::string p1 = testutil::temp_path("lossy_t1.prmd").string();
      const std::string p6 = testutil::temp_path("lossy_t6.prmd").string();
      encode_file(src, p1, Codec::Prmd, o);
      o.threads = 6;
      encode_file(src, p6, Codec::Prmd, o);
      ASSERT_EQ(testutil::read_file(p1), testutil::read_file(p6));
      auto d1 = open_decoder(p1);
      auto d6 = open_decoder(p6);
      for (int L = 0; L < 5; ++L) {
        const LevelInfo li = d1->level_info(L);
        const Rect all{0, 0, li.width, li.height};
        ASSERT_EQ(d6->decode(L, all).image.pixels,
                  d1->decode(L, all).image.pixels);
      }
    }
  }
}

TEST(ThreadsRoundTrip, ConcurrentEncodesFromUserThreadsAreIndependent) {
  // Four user threads, each encoding with its own pool of 3.
  const char* names[] = {"haar_300x200_rgb", "cdf53_q8_rgb",
                         "lapgauss_257x129_gray", "naive_1000x777_gray",
                         "haar_range_300x200_gray", "lapbox_q3_rgb"};
  std::vector<Case> cs;
  for (const char* n : names) {
    cs.push_back(case_named(n));
    (void)source_of(cs.back());  // fill the cache before the threads start
  }
  std::vector<uint64_t> got(cs.size());
  std::vector<std::thread> users;
  std::atomic<size_t> next{0};
  for (int u = 0; u < 4; ++u) {
    users.emplace_back([&, u] {
      for (size_t i; (i = next.fetch_add(1)) < cs.size();) {
        got[i] = encode_case(cs[i], 3, "user" + std::to_string(u)).hash;
      }
    });
  }
  for (auto& t : users) {
    t.join();
  }
  for (size_t i = 0; i < cs.size(); ++i) {
    EXPECT_EQ(got[i], golden().at(cs[i].name)) << cs[i].name;
  }
}

// ----------------------------------------------------------------------------
// Failures: the same error at every thread count, and no output file

namespace {

// zstd coder that throws for the blocks selected by `fails` (a property of
// the block's samples, so the same blocks fail whatever thread codes them).
// The message names the block, so the first failure in file order is
// identifiable.
class FailingCoder final : public internal::BlockCoder {
 public:
  using Pred = bool (*)(const internal::BlockPlanes&);
  FailingCoder(Pred fails, bool invalid_arg)
      : inner_(internal::make_block_coder(Entropy::Zstd)),
        fails_(fails),
        invalid_arg_(invalid_arg) {}

  Entropy id() const override { return Entropy::Zstd; }

  void encode(const internal::BlockPlanes& in,
              std::vector<uint8_t>& out) const override {
    calls_.fetch_add(1);
    if (fails_(in)) {
      std::string what = "injected failure: " + std::to_string(in.w) + "x" +
                         std::to_string(in.h) + " sum " +
                         std::to_string(checksum(in));
      if (invalid_arg_) {
        throw std::invalid_argument(what);
      }
      throw std::runtime_error(what);
    }
    inner_->encode(in, out);
  }

  void decode(const uint8_t* data,
              size_t size,
              internal::BlockPlanes& io) const override {
    inner_->decode(data, size, io);
  }

  static uint64_t checksum(const internal::BlockPlanes& in) {
    uint64_t s = 0;
    for (const auto& p : in.planes) {
      for (size_t i = 0; i < p.size(); ++i) {
        s = s * 31 + uint64_t(int64_t(p[i]));
      }
    }
    return s;
  }

  int calls() const { return calls_.load(); }

 private:
  std::unique_ptr<internal::BlockCoder> inner_;
  Pred fails_;
  bool invalid_arg_;
  mutable std::atomic<int> calls_{0};
};

// Encodes `c` with `coder` at `threads`; returns the exception message (empty
// if none) and checks that no file was left behind.
std::string failing_encode(const Case& c,
                           const internal::BlockCoder& coder,
                           int threads) {
  EncodeOptions o = c.opts;
  o.threads = threads;
  const std::string path = testutil::temp_path("fail.prmd").string();
  std::filesystem::remove(path);
  std::string msg;
  try {
    internal::encode_prmd(source_of(c), path, o, &coder);
  } catch (const std::runtime_error& e) {
    msg = std::string("runtime_error: ") + e.what();
  } catch (const std::invalid_argument& e) {
    msg = std::string("invalid_argument: ") + e.what();
  }
  EXPECT_FALSE(std::filesystem::exists(path))
      << "partial output left behind at threads " << threads;
  return msg;
}

bool fails_some(const internal::BlockPlanes& in) {
  // Roughly a third of the blocks, spread over every level.
  return (FailingCoder::checksum(in) & 3) == 1;
}
bool fails_all(const internal::BlockPlanes&) {
  return true;
}
bool fails_full_size_detail(const internal::BlockPlanes& in) {
  // Only full-size level-0 detail blocks: the last level in file order.
  return in.w == 16 && in.h == 16 && in.kinds[0] != internal::Band::Base;
}

}  // namespace

TEST(ThreadsErrors, SameExceptionAtEveryThreadCount) {
  const Case c = case_named("haar_1000x777_rgb");  // ~250 blocks
  struct Scenario {
    FailingCoder::Pred pred;
    bool invalid_arg;
  };
  for (const Scenario s : {Scenario{fails_some, false},
                           Scenario{fails_all, true},
                           Scenario{fails_some, true}}) {
    const FailingCoder serial_coder(s.pred, s.invalid_arg);
    const std::string serial = failing_encode(c, serial_coder, 1);
    ASSERT_FALSE(serial.empty()) << "no block failed";
    for (const int n : {2, 3, 7, 16, 0}) {
      for (int rep = 0; rep < 5; ++rep) {
        const FailingCoder coder(s.pred, s.invalid_arg);
        EXPECT_EQ(failing_encode(c, coder, n), serial) << "threads " << n;
      }
    }
  }
}

TEST(ThreadsErrors, FailureInTheLastLevelStillWins) {
  // Only level-0 blocks fail, so every coarser block is coded first; the
  // first failing level-0 block in id order must be reported.
  Case c = case_named("haar_1000x777_gray");
  c.opts.cblk = 32;
  c.opts.tile = 64;
  const FailingCoder serial_coder(fails_full_size_detail, false);
  const std::string serial = failing_encode(c, serial_coder, 1);
  ASSERT_FALSE(serial.empty());
  for (const int n : {2, 5, 16}) {
    const FailingCoder coder(fails_full_size_detail, false);
    EXPECT_EQ(failing_encode(c, coder, n), serial) << n;
  }
}

TEST(ThreadsErrors, ExistingFileIsLeftUntouched) {
  const Case c = case_named("cdf53_300x200_gray");
  const std::string path = testutil::temp_path("keep.prmd").string();
  const std::vector<char> old = {'o', 'l', 'd'};
  for (const int n : {1, 4}) {
    testutil::write_file(path, old);
    EncodeOptions o = c.opts;
    o.threads = n;
    const FailingCoder coder(fails_all, false);
    EXPECT_THROW(internal::encode_prmd(source_of(c), path, o, &coder),
                 std::runtime_error);
    EXPECT_EQ(testutil::read_file(path), old) << n;
  }
}

TEST(ThreadsErrors, ParallelCodingStopsEarly) {
  // After the first failure unclaimed blocks are skipped, so far fewer than
  // all blocks are coded when block 0 fails.
  Case c = case_named("haar_1000x777_gray");
  c.opts.cblk = 16;
  c.opts.tile = 16;
  const int blocks = present_blocks(c);
  const FailingCoder coder(fails_all, false);
  failing_encode(c, coder, 4);
  EXPECT_LT(coder.calls(), blocks / 2);
}

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool kThreadSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kThreadSanitizer = true;
#else
constexpr bool kThreadSanitizer = false;
#endif
#else
constexpr bool kThreadSanitizer = false;
#endif

// Threads of this process (Linux: entries of /proc/self/task), 0 if unknown.
int process_threads() {
  std::error_code ec;
  int n = 0;
  for (std::filesystem::directory_iterator it("/proc/self/task", ec), end;
       !ec && it != end; it.increment(ec)) {
    ++n;
  }
  return ec ? 0 : n;
}

// zstd coder that records which threads coded blocks, and the process's
// thread count while it runs.
class ThreadRecordingCoder final : public internal::BlockCoder {
 public:
  ThreadRecordingCoder() : inner_(internal::make_block_coder(Entropy::Zstd)) {}
  Entropy id() const override { return Entropy::Zstd; }
  void encode(const internal::BlockPlanes& in,
              std::vector<uint8_t>& out) const override {
    {
      const std::lock_guard<std::mutex> lock(m_);
      ids_.insert(std::this_thread::get_id());
      max_threads_ = std::max(max_threads_, process_threads());
    }
    inner_->encode(in, out);
  }
  void decode(const uint8_t* d,
              size_t n,
              internal::BlockPlanes& io) const override {
    inner_->decode(d, n, io);
  }
  std::set<std::thread::id> ids() const {
    const std::lock_guard<std::mutex> lock(m_);
    return ids_;
  }
  int max_threads() const {
    const std::lock_guard<std::mutex> lock(m_);
    return max_threads_;
  }

 private:
  std::unique_ptr<internal::BlockCoder> inner_;
  mutable std::mutex m_;
  mutable std::set<std::thread::id> ids_;
  mutable int max_threads_ = 0;
};

}  // namespace

TEST(ThreadsSerial, OneThreadStartsNoThreadAndMatchesTheGoldenFile) {
  const Case c = case_named("haar_1000x777_rgb");
  EncodeOptions o = c.opts;
  o.threads = 1;
  const std::string path = testutil::temp_path("serial.prmd").string();
  const ThreadRecordingCoder serial;
  const int before = process_threads();
  const EncodeReport r = internal::encode_prmd(source_of(c), path, o, &serial);
  EXPECT_EQ(r.threads, 1);
  EXPECT_EQ(file_hash(path), golden().at(c.name));
  // Every block on the calling thread.
  EXPECT_EQ(serial.ids(),
            std::set<std::thread::id>{std::this_thread::get_id()});

  // The same with 4 threads: same file, at most 4 coding threads.
  o.threads = 4;
  const ThreadRecordingCoder mt;
  EXPECT_EQ(internal::encode_prmd(source_of(c), path, o, &mt).threads, 4);
  EXPECT_EQ(file_hash(path), golden().at(c.name));
  EXPECT_LE(mt.ids().size(), 4u);
  if (before > 0 && !kThreadSanitizer) {
    // The process thread count while blocks were coded: the serial encode
    // added none, the 4-thread one exactly 3 workers, joined at the end.
    // (TSan starts a helper thread with the first thread created.)
    EXPECT_EQ(serial.max_threads(), before);
    EXPECT_EQ(mt.max_threads(), before + 3);
    EXPECT_EQ(process_threads(), before);
  }
}

TEST(ThreadsFallback, FewerWorkersThanRequestedStillWriteTheSameFile) {
  // The system refuses threads after k workers (test hook): the encode runs
  // on the threads it got, reports them, and writes the same file.
  struct Reset {
    ~Reset() { internal::test_hooks::set_worker_start_limit(-1); }
  } reset;
  const Case c = case_named("cdf53_1000x777_rgb");
  for (const int k : {0, 1, 2}) {
    internal::test_hooks::set_worker_start_limit(k);
    for (const int n : {4, 16}) {
      const Encoded e = encode_case(c, n, "fallback");
      EXPECT_EQ(e.report.threads, 1 + k) << "k " << k << " threads " << n;
      EXPECT_EQ(e.hash, golden().at(c.name)) << "k " << k << " threads " << n;
    }
  }
  internal::test_hooks::set_worker_start_limit(-1);
  EXPECT_EQ(encode_case(c, 4, "nofallback").report.threads, 4);
}

// ----------------------------------------------------------------------------
// J2K

TEST(ThreadsJ2k, OutputIsIdenticalAcrossThreadCounts) {
  for (const ColorFormat f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
    for (const bool lossless : {true, false}) {
      const Image src = ref::synthetic_image(900, 611, f, 4);
      EncodeOptions o;
      o.levels = 4;
      o.color = f;
      o.j2k_tile = 512;  // 64 code-blocks of 64²: room for 16 threads
      o.j2k_lossless = lossless;
      o.j2k_psnr = lossless ? 0.f : 38.f;
      std::vector<char> first;
      for (const int n : {1, 2, 3, 8, 16, 0}) {
        o.threads = n;
        const std::string path = testutil::temp_path("t.j2k").string();
        const EncodeReport r = encode_file(src, path, Codec::J2K, o);
        if (n == 1) {
          EXPECT_EQ(r.threads, 1);
        } else {
          EXPECT_GE(r.threads, 1);
          EXPECT_LE(r.threads, 16);  // (512 / 64)² / 4
        }
        const std::vector<char> bytes = testutil::read_file(path);
        if (first.empty()) {
          first = bytes;
        } else {
          EXPECT_EQ(bytes, first) << info(f).name << " lossless=" << lossless
                                  << " threads=" << n;
        }
      }
    }
  }
}

TEST(ThreadsJ2k, ThreadsAreCappedByCodeBlocksPerTile) {
  const Image src = ref::synthetic_image(600, 400, ColorFormat::Gray8, 4);
  EncodeOptions o;
  o.levels = 3;
  o.threads = 16;
  const std::string path = testutil::temp_path("cap.j2k").string();
  o.j2k_tile = 256;  // (256/64)² = 16 code-blocks per tile → 4 threads
  EXPECT_EQ(encode_file(src, path, Codec::J2K, o).threads, 4);
  o.j2k_tile = 64;  // one code-block per tile → serial
  EXPECT_EQ(encode_file(src, path, Codec::J2K, o).threads, 1);
  o.threads = 1;
  o.j2k_tile = 1024;
  EXPECT_EQ(encode_file(src, path, Codec::J2K, o).threads, 1);
}
