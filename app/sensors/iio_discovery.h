/**
 * @file iio_discovery.h
 * 根据 IIO name 属性寻找设备，同时返回 sysfs 路径和 /dev/iio:deviceN 节点。
 * 设备编号受探测顺序影响，不能写死；自动发现只接受唯一匹配，多个同名设备需要显式配置。
 */
#ifndef OCR_SENSORS_IIO_DISCOVERY_H
#define OCR_SENSORS_IIO_DISCOVERY_H

#include <stddef.h>

/**
 * Find an IIO device by the exact contents of its sysfs `name` attribute.
 * Either output may be omitted by passing NULL and size 0.
 *
 * @return 0 on success, a negative value when invalid, unavailable or too long.
 *
 * 按 name 精确匹配唯一 IIO 设备，同时构造 sysfs 和字符设备路径。
 */
int ocr_iio_find_device(const char *iio_name,
                        char *sysfs_path, size_t sysfs_size,
                        char *dev_path, size_t dev_size);

#endif /* OCR_SENSORS_IIO_DISCOVERY_H */
