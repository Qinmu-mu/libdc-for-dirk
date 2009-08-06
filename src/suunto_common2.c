/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcmp, memcpy
#include <assert.h> // assert

#include "suunto_common2.h"
#include "utils.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "array.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define MAXRETRIES 2

#define SZ_VERSION    0x04
#define SZ_MEMORY     0x8000
#define SZ_PACKET     0x78
#define SZ_MINIMUM    8

#define FP_OFFSET     0x15

#define RB_PROFILE_BEGIN            0x019A
#define RB_PROFILE_END              SZ_MEMORY - 2
#define RB_PROFILE_DISTANCE(a,b)    ringbuffer_distance (a, b, RB_PROFILE_BEGIN, RB_PROFILE_END)

#define BACKEND(abstract)	((suunto_common2_device_backend_t *) abstract->backend)

void
suunto_common2_device_init (suunto_common2_device_t *device, const suunto_common2_device_backend_t *backend)
{
	assert (device != NULL);

	// Initialize the base class.
	device_init (&device->base, &backend->base);

	// Set the default values.
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
}


static device_status_t
suunto_common2_transfer (device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 4);

	if (BACKEND (abstract)->packet == NULL)
		return DEVICE_STATUS_UNSUPPORTED;

	// Occasionally, the dive computer does not respond to a command.
	// In that case we retry the command a number of times before
	// returning an error. Usually the dive computer will respond
	// again during one of the retries.

	unsigned int nretries = 0;
	device_status_t rc = DEVICE_STATUS_SUCCESS;
	while ((rc = BACKEND (abstract)->packet (abstract, command, csize, answer, asize, size)) != DEVICE_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DEVICE_STATUS_TIMEOUT && rc != DEVICE_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;
	}

	return rc;
}


device_status_t
suunto_common2_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_common2_device_t *device = (suunto_common2_device_t*) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_common2_device_version (device_t *abstract, unsigned char data[], unsigned int size)
{
	if (size < SZ_VERSION) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	unsigned char answer[SZ_VERSION + 4] = {0};
	unsigned char command[4] = {0x0F, 0x00, 0x00, 0x0F};
	device_status_t rc = suunto_common2_transfer (abstract, command, sizeof (command), answer, sizeof (answer), 4);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer + 3, SZ_VERSION);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_common2_device_reset_maxdepth (device_t *abstract)
{
	unsigned char answer[4] = {0};
	unsigned char command[4] = {0x20, 0x00, 0x00, 0x20};
	device_status_t rc = suunto_common2_transfer (abstract, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_common2_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size, device_progress_t *progress)
{
	// The data transmission is split in packages
	// of maximum $SZ_PACKET bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = size - nbytes;
		if (len > SZ_PACKET)
			len = SZ_PACKET;

		// Read the package.
		unsigned char answer[SZ_PACKET + 7] = {0};
		unsigned char command[7] = {0x05, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[6] = checksum_xor_uint8 (command, 6, 0x00);
		device_status_t rc = suunto_common2_transfer (abstract, command, sizeof (command), answer, len + 7, len);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 6, len);

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, progress);
		}

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_common2_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	return suunto_common2_read (abstract, address, data, size, NULL);
}


device_status_t
suunto_common2_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	// The data transmission is split in packages
	// of maximum $SZ_PACKET bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = size - nbytes;
		if (len > SZ_PACKET)
			len = SZ_PACKET;

		// Write the package.
		unsigned char answer[7] = {0};
		unsigned char command[SZ_PACKET + 7] = {0x06, 0x00, len + 3,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (command + 6, data, len);
		command[len + 6] = checksum_xor_uint8 (command, len + 6, 0x00);
		device_status_t rc = suunto_common2_transfer (abstract, command, len + 7, answer, sizeof (answer), 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_common2_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	if (size < SZ_MEMORY) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	device_status_t rc = suunto_common2_read (abstract, 0x00, data, SZ_MEMORY, &progress);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	if (result)
		*result = SZ_MEMORY;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_common2_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	suunto_common2_device_t *device = (suunto_common2_device_t*) abstract;

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = RB_PROFILE_END - RB_PROFILE_BEGIN + 8 + SZ_VERSION + (SZ_MINIMUM > 4 ? SZ_MINIMUM : 4);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Read the version info.
	unsigned char version[SZ_VERSION] = {0};
	device_status_t rc = suunto_common2_device_version (abstract, version, sizeof (version));
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read memory header.");
		return rc;
	}

	// Update and emit a progress event.
	progress.current += sizeof (version);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Read the serial number.
	unsigned char serial[SZ_MINIMUM > 4 ? SZ_MINIMUM : 4] = {0};
	rc = suunto_common2_read (abstract, 0x0023, serial, sizeof (serial), NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read memory header.");
		return rc;
	}

	// Update and emit a progress event.
	progress.current += sizeof (serial);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = version[0];
	devinfo.firmware = array_uint24_be (version + 1);
	devinfo.serial = array_uint32_be (serial);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	// Read the header bytes.
	unsigned char header[8] = {0};
	rc = suunto_common2_read (abstract, 0x0190, header, sizeof (header), NULL);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Cannot read memory header.");
		return rc;
	}

	// Obtain the pointers from the header.
	unsigned int last  = array_uint16_le (header + 0);
	unsigned int count = array_uint16_le (header + 2);
	unsigned int end   = array_uint16_le (header + 4);
	unsigned int begin = array_uint16_le (header + 6);

	// Memory buffer to store all the dives.

	unsigned char data[SZ_MINIMUM + RB_PROFILE_END - RB_PROFILE_BEGIN] = {0};

	// Calculate the total amount of bytes.

	unsigned int remaining = RB_PROFILE_DISTANCE (begin, end);

	// Update and emit a progress event.

	progress.maximum -= (RB_PROFILE_END - RB_PROFILE_BEGIN) - remaining;
	progress.current += sizeof (header);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// To reduce the number of read operations, we always try to read
	// packages with the largest possible size. As a consequence, the
	// last package of a dive can contain data from more than one dive.
	// Therefore, the remaining data of this package (and its size)
	// needs to be preserved for the next dive.

	unsigned int available = 0;

	// The ring buffer is traversed backwards to retrieve the most recent
	// dives first. This allows you to download only the new dives. During
	// the traversal, the current pointer does always point to the end of
	// the dive data and we move to the "next" dive by means of the previous
	// pointer.

	unsigned int ndives = 0;
	unsigned int current = end;
	unsigned int previous = last;
	while (current != begin) {
		// Calculate the size of the current dive.
		unsigned int size = RB_PROFILE_DISTANCE (previous, current);

		assert (size >= 4 && size <= remaining);

		unsigned int nbytes = available;
		unsigned int address = current - available;
		while (nbytes < size) {
			// Calculate the package size. Try with the largest possible
			// size first, and adjust when the end of the ringbuffer or
			// the end of the profile data is reached.
			unsigned int len = SZ_PACKET;
			if (RB_PROFILE_BEGIN + len > address)
				len = address - RB_PROFILE_BEGIN; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.
			/*if (nbytes + len > size)
				len = size - nbytes;*/ // End of dive (for testing only).

			// Always read at least the minimum amount of bytes, because
			// reading fewer bytes is unreliable. The memory buffer is
			// large enough to prevent buffer overflows, and the extra
			// bytes are automatically ignored (due to reading backwards).
			unsigned int extra = 0;
			if (len < SZ_MINIMUM)
				extra = SZ_MINIMUM - len;

			// Read the package.
			unsigned char *p = data + SZ_MINIMUM + remaining - nbytes;
			rc = suunto_common2_read (abstract, address - (len + extra), p - (len + extra), len + extra, NULL);
			if (rc != DEVICE_STATUS_SUCCESS) {
				WARNING ("Cannot read memory.");
				return rc;
			}

			// Update and emit a progress event.
			progress.current += len;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

			// Next package.
			nbytes += len;
			address -= len;
			if (address <= RB_PROFILE_BEGIN)
				address = RB_PROFILE_END;
		}

		// The last package of the current dive contains the previous and
		// next pointers (in a continuous memory area). It can also contain
		// a number of bytes from the next dive. The offset to the pointers
		// is equal to the number of bytes remaining after the current dive.

		remaining -= size;
		available = nbytes - size;

		unsigned int oprevious = array_uint16_le (data + SZ_MINIMUM + remaining + 0);
		unsigned int onext     = array_uint16_le (data + SZ_MINIMUM + remaining + 2);
		assert (current == onext);

		// Next dive.
		current = previous;
		previous = oprevious;
		ndives++;

		unsigned int offset = SZ_MINIMUM + remaining;
		if (memcmp (data + offset + FP_OFFSET, device->fingerprint, sizeof (device->fingerprint)) == 0)
			return DEVICE_STATUS_SUCCESS;

		if (callback && !callback (data + offset + 4, size - 4, userdata))
			return DEVICE_STATUS_SUCCESS;
	}
	assert (remaining == 0);
	assert (available == 0);
	assert (ndives == count);

	return DEVICE_STATUS_SUCCESS;
}
