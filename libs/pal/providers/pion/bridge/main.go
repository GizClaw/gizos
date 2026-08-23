package main

/*
#cgo CFLAGS: -I${SRCDIR}/../src
#include "h2_pion_bridge.h"
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"math"
	"runtime/cgo"
	"unsafe"

	"h2vivi/firmwares/libs/pal/providers/pion"
)

const (
	resultOK           = 0
	resultInvalidArg   = -1
	resultIO           = -4
	resultNoMemory     = -5
	resultInvalidState = -7
	resultWouldBlock   = -9
	resultClosed       = -10
	resultFull         = -11
	resultNoSpace      = -13
)

//export h2PionGoPeerCreate
func h2PionGoPeerCreate() C.uint64_t {
	backend, err := pion.New()
	if err != nil {
		return 0
	}
	return C.uint64_t(cgo.NewHandle(backend))
}

//export h2PionGoPeerDestroy
func h2PionGoPeerDestroy(handle C.uint64_t) {
	if handle == 0 {
		return
	}
	h := cgo.Handle(handle)
	h.Value().(*pion.Backend).Close()
	h.Delete()
}

//export h2PionGoPeerAddICEServer
func h2PionGoPeerAddICEServer(
	handle C.uint64_t,
	url *C.char,
	urlLen C.size_t,
	username *C.char,
	usernameLen C.size_t,
	credential *C.char,
	credentialLen C.size_t,
) C.int {
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	urlString, ok := stringFromC(url, urlLen)
	if !ok || urlString == "" {
		return resultInvalidArg
	}
	usernameString, ok := stringFromC(username, usernameLen)
	if !ok {
		return resultInvalidArg
	}
	credentialString, ok := stringFromC(credential, credentialLen)
	if !ok {
		return resultInvalidArg
	}
	return resultFor(backend.AddICEServer(urlString, usernameString, credentialString))
}

//export h2PionGoPeerStartOffer
func h2PionGoPeerStartOffer(handle C.uint64_t, outSDP **C.char, outLen *C.size_t) C.int {
	if outSDP == nil || outLen == nil {
		return resultInvalidArg
	}
	*outSDP = nil
	*outLen = 0
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	sdp, err := backend.StartOffer()
	if err != nil {
		return resultFor(err)
	}
	allocated := C.CBytes([]byte(sdp))
	if allocated == nil && len(sdp) != 0 {
		return resultNoMemory
	}
	*outSDP = (*C.char)(allocated)
	*outLen = C.size_t(len(sdp))
	return resultOK
}

//export h2PionGoPeerSetRemoteSDP
func h2PionGoPeerSetRemoteSDP(handle C.uint64_t, sdp *C.char, sdpLen C.size_t) C.int {
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	answer, ok := stringFromC(sdp, sdpLen)
	if !ok {
		return resultInvalidArg
	}
	return resultFor(backend.SetRemoteAnswer(answer))
}

//export h2PionGoPeerCreateDataChannel
func h2PionGoPeerCreateDataChannel(
	handle C.uint64_t,
	channelKey C.uint64_t,
	label *C.char,
	labelLen C.size_t,
	hasStreamID C.int,
	streamID C.uint16_t,
	ordered C.int,
	reliable C.int,
) C.int {
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	labelString, ok := stringFromC(label, labelLen)
	if !ok || labelString == "" {
		return resultInvalidArg
	}
	err := backend.CreateDataChannel(
		uint64(channelKey),
		labelString,
		hasStreamID != 0,
		uint16(streamID),
		ordered != 0,
		reliable != 0,
	)
	return resultFor(err)
}

//export h2PionGoPeerPoll
func h2PionGoPeerPoll(handle C.uint64_t, peerKey C.uintptr_t, timeoutMS C.int) C.int {
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	events, overflow := backend.Poll(int(timeoutMS))
	for _, event := range events {
		if result := dispatchEvent(peerKey, event); result != resultOK {
			return result
		}
	}
	if overflow {
		return resultFull
	}
	return resultOK
}

//export h2PionGoChannelSend
func h2PionGoChannelSend(
	handle C.uint64_t,
	channelKey C.uint64_t,
	data *C.uint8_t,
	dataLen C.size_t,
	isText C.int,
) C.int {
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	payload, ok := bytesFromC(data, dataLen)
	if !ok {
		return resultInvalidArg
	}
	return resultFor(backend.Send(uint64(channelKey), payload, isText != 0))
}

//export h2PionGoPeerSendOpus
func h2PionGoPeerSendOpus(handle C.uint64_t, data *C.uint8_t, dataLen C.size_t) C.int {
	backend, ok := backendFor(handle)
	if !ok {
		return resultInvalidState
	}
	payload, ok := bytesFromC(data, dataLen)
	if !ok || len(payload) == 0 {
		return resultInvalidArg
	}
	return resultFor(backend.SendOpus(payload))
}

//export h2PionGoChannelClose
func h2PionGoChannelClose(handle C.uint64_t, channelKey C.uint64_t) {
	backend, ok := backendFor(handle)
	if ok {
		backend.CloseChannel(uint64(channelKey))
	}
}

func backendFor(handle C.uint64_t) (backend *pion.Backend, ok bool) {
	if handle == 0 {
		return nil, false
	}
	defer func() {
		if recover() != nil {
			backend = nil
			ok = false
		}
	}()
	backend, ok = cgo.Handle(handle).Value().(*pion.Backend)
	return backend, ok
}

func stringFromC(value *C.char, length C.size_t) (string, bool) {
	if uint64(length) > math.MaxInt32 || (value == nil && length != 0) {
		return "", false
	}
	if length == 0 {
		return "", true
	}
	return C.GoStringN(value, C.int(length)), true
}

func bytesFromC(value *C.uint8_t, length C.size_t) ([]byte, bool) {
	if uint64(length) > math.MaxInt32 || (value == nil && length != 0) {
		return nil, false
	}
	if length == 0 {
		return nil, true
	}
	return C.GoBytes(unsafe.Pointer(value), C.int(length)), true
}

func resultFor(err error) C.int {
	if err == nil {
		return resultOK
	}
	if errors.Is(err, pion.ErrWouldBlock) {
		return resultWouldBlock
	}
	if errors.Is(err, pion.ErrNoSpace) {
		return resultNoSpace
	}
	if errors.Is(err, pion.ErrClosed) {
		return resultClosed
	}
	if errors.Is(err, pion.ErrInvalidState) {
		return resultInvalidState
	}
	return resultIO
}

func dispatchEvent(peerKey C.uintptr_t, event pion.Event) C.int {
	switch event.Kind {
	case pion.EventPeerState:
		C.h2_pion_bridge_emit_peer_state(peerKey, C.int(event.State))
	case pion.EventChannelOpen:
		label := []byte(event.Label)
		var labelData *C.char
		if len(label) != 0 {
			labelData = (*C.char)(unsafe.Pointer(&label[0]))
		}
		return C.h2_pion_bridge_emit_channel_open(
			peerKey,
			C.uint64_t(event.ChannelKey),
			labelData,
			C.size_t(len(label)),
			boolInt(event.HasStreamID),
			C.uint16_t(event.StreamID),
			boolInt(event.Ordered),
			boolInt(event.Reliable),
			boolInt(event.Remote),
		)
	case pion.EventChannelState:
		C.h2_pion_bridge_emit_channel_state(peerKey, C.uint64_t(event.ChannelKey), C.int(event.State))
	case pion.EventChannelMessage:
		var data *C.uint8_t
		if len(event.Data) != 0 {
			data = (*C.uint8_t)(unsafe.Pointer(&event.Data[0]))
		}
		C.h2_pion_bridge_emit_channel_message(
			peerKey,
			C.uint64_t(event.ChannelKey),
			data,
			C.size_t(len(event.Data)),
			boolInt(event.IsText),
		)
	case pion.EventOpusFrame:
		if len(event.Data) != 0 {
			C.h2_pion_bridge_emit_opus_frame(
				peerKey,
				(*C.uint8_t)(unsafe.Pointer(&event.Data[0])),
				C.size_t(len(event.Data)),
			)
		}
	}
	return resultOK
}

func boolInt(value bool) C.int {
	if value {
		return 1
	}
	return 0
}

func main() {}
