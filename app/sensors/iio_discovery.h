#ifndef OCR_SENSORS_IIO_DISCOVERY_H
#define OCR_SENSORS_IIO_DISCOVERY_H

#include <stddef.h>

/**
 * Find an IIO device by the exact contents of its sysfs `name` attribute.
 * Either output may be omitted by passing NULL and size 0.
 *
 * @return 0 on success, a negative value when invalid, unavailable or too long.
 */
int ocr_iio_find_device(const char *iio_name,
                        char *sysfs_path, size_t sysfs_size,
                        char *dev_path, size_t dev_size);

#endif /* OCR_SENSORS_IIO_DISCOVERY_H */
