/* What the kernel's accelerator (SweetAccel.cpp) and its daemon (sweetaccel-d) pass to each other. */
#ifndef SWEETACCEL_PROTO_H
#define SWEETACCEL_PROTO_H
#define ACC_MAXIN 2048          /* structure bytes in either direction; the call's own limit is 4096 */
#define ACC_SCALARS 16

/* control selector 0: wait for the next request of a program (answers kIOReturnTimeout after 5 s) */
struct accel_ctrl_req {
	uint32_t slot, selector, type, pid, nsin, insize, outwanted, pad;
	uint64_t sin[ACC_SCALARS];
	uint8_t  in[ACC_MAXIN];
};

/* control selector 1: answer the request in `slot` */
struct accel_ctrl_reply {
	uint32_t slot, nsout, outsize;
	int32_t  result;
	uint64_t sout[ACC_SCALARS];
	uint8_t  out[ACC_MAXIN];
};
#endif
