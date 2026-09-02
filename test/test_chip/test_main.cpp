/**
 * 示例单元测试 —— 用 `pio test` 运行。
 *
 * PlatformIO 会把这个文件编译成一个独立固件烧到板子上执行，
 * 结果通过串口回传到终端。也就是说这些断言是在真实 ESP8285 上跑的，
 * 可以用来验证"手上这块芯片和 platformio.ini 的配置对得上"。
 */

#include <Arduino.h>
#include <unity.h>

void setUp(void) {
  // 每个 test 前执行，这里用不上
}

void tearDown(void) {
  // 每个 test 后执行，这里用不上
}

/** ESP8285 内置的就是 1MB Flash，对不上说明 board 选错了。 */
void test_flash_is_1mb(void) {
  TEST_ASSERT_EQUAL_UINT32(1048576UL, ESP.getFlashChipRealSize());
}

/** 分区脚本声明的大小不能超过实际 Flash，否则运行时写 Flash 会炸。 */
void test_flash_config_fits(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(ESP.getFlashChipRealSize(),
                                   ESP.getFlashChipSize());
}

/** 刚启动时堆内存应该还很宽裕，太低说明有东西吃掉了内存。 */
void test_heap_is_healthy(void) {
  TEST_ASSERT_GREATER_THAN_UINT32(10000UL, ESP.getFreeHeap());
}

/** platformio.ini 里 board_build.f_cpu 设的是 80MHz。 */
void test_cpu_freq(void) {
  TEST_ASSERT_EQUAL_UINT8(80, ESP.getCpuFreqMHz());
}

void setup() {
  // 等一下再开始，否则测试运行器可能还没连上串口，前几行结果会丢
  delay(2000);

  UNITY_BEGIN();
  RUN_TEST(test_flash_is_1mb);
  RUN_TEST(test_flash_config_fits);
  RUN_TEST(test_heap_is_healthy);
  RUN_TEST(test_cpu_freq);
  UNITY_END();
}

void loop() {
  // UNITY_END() 之后不需要做什么
}
