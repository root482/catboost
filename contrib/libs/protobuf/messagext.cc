#include "io/coded_stream.h"
#include "io/zero_copy_stream_impl.h"
#include "messagext.h"
#include "messageint.h"

#include <util/ysaveload.h>
#include <util/generic/yexception.h>
#include <util/stream/length.h>

namespace {
    const int MaxSizeBytes = 1 << 27; // 128 MB limits the size of a protobuf message processed by TProtoSerializer
    const int WarningSizeBytes = -1;  // Disabled warning message after a certain size threshold
}

namespace google {
namespace protobuf {
namespace io {

bool ParseFromCodedStreamSeq(Message* msg, io::CodedInputStream* input) {
    msg->Clear();
    ui32 size;
    if (!input->ReadVarint32(&size)) {
        return false;
    }

    io::CodedInputStream::Limit limitState = input->PushLimit(size);
    bool res = msg->ParseFromCodedStream(input);
    input->PopLimit(limitState);
    return res;
}

bool ParseFromZeroCopyStreamSeq(Message* msg, io::ZeroCopyInputStream* input) {
    io::CodedInputStream decoder(input);
    return ParseFromCodedStreamSeq(msg, &decoder);
}

bool SerializePartialToCodedStreamSeq(const Message* msg, io::CodedOutputStream* output) {
    uint32 size = msg->ByteSize();  // Force size to be cached.
    output->WriteVarint32(size);
    msg->SerializeWithCachedSizes(output);
    return !output->HadError();
}

bool SerializeToCodedStreamSeq(const Message* msg, io::CodedOutputStream* output) {
    GOOGLE_DCHECK(msg->IsInitialized()) << InitializationErrorMessage("serialize", *msg);
    return SerializePartialToCodedStreamSeq(msg, output);
}

bool SerializeToZeroCopyStreamSeq(const Message* msg, io::ZeroCopyOutputStream* output) {
    io::CodedOutputStream encoder(output);
    return SerializeToCodedStreamSeq(msg, &encoder);
}


int TInputStreamProxy::Read(void* buffer, int size) {
    try {
        return (int)mSlave->Read(buffer, (size_t)size);
    } catch (const yexception& e) {
        GOOGLE_LOG(ERROR) << e.what();
    } catch (...) {
        GOOGLE_LOG(ERROR) << "unknown exception caught";
    }
    TErrorState::SetError();
    return -1;
}


bool TOutputStreamProxy::Write(const void* buffer, int size) {
    try {
        mSlave->Write(buffer, (size_t)size);
        return true;
    } catch (const yexception& e) {
        GOOGLE_LOG(ERROR) << e.what();
    } catch (...) {
        GOOGLE_LOG(ERROR) << "unknown exception caught";
    }
    TErrorState::SetError();
    return false;
}


void TProtoSerializer::Save(TOutputStream* out, const Message& msg) {
    int size = msg.ByteSize();
    if (size > MaxSizeBytes) {
        ythrow yexception() << "Message size " << size << " exceeds " << MaxSizeBytes;
    }

    TCopyingOutputStreamAdaptor adaptor(out);
    io::CodedOutputStream encoder(&adaptor);
    if (!SerializeToCodedStreamSeq(&msg, &encoder))
        ythrow yexception() << "Cannot write protobuf::Message to output stream";
}

// Reading varint32 directly from TInputStream (might be slow if input requires buffering).
// Copy-pasted with small modifications from protobuf/io/coded_stream (ReadVarint32FromArray)

// Returns true if succeeded, false if stream has ended, throws exception if data is corrupted
static bool ReadVarint32(TInputStream* input, ui32& size) {
    size_t res;
    ui8 b;

    /* if we can't read anything from stream - it is exhausted */
    if (input->Read(&b, 1) == 0)
        return false;
                      res  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
    ::Load(input, b); res |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
    ::Load(input, b); res |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
    ::Load(input, b); res |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
    ::Load(input, b); res |=  b         << 28; if (!(b & 0x80)) goto done;

    // If the input is larger than 32 bits, we still need to read it all
    // and discard the high-order bits.
    for (int i = 0; i < 5; i++) {
        ::Load(input, b); if (!(b & 0x80)) goto done;
    }
    ythrow yexception() << "We have overrun the maximum size of a varint (10 bytes).  Assume the data is corrupt.";

done:
    size = res;
    return true;
}

class TTempBufHelper {
public:
    TTempBufHelper(size_t size) {
        if (size <= SmallBufSize) {
            Buffer = SmallBuf;
        } else {
            LargeBuf.Reset(new TTempBuf(size));
            Buffer = reinterpret_cast<ui8*>(LargeBuf->Data());
        }
    }

    ui8* Data() {
        return Buffer;
    }

private:
    static const size_t SmallBufSize = 1024;
    ui8 SmallBuf[SmallBufSize];

    THolder<TTempBuf> LargeBuf;

    ui8* Buffer;
};

void TProtoSerializer::Load(TInputStream* input, Message& msg) {
    ui32 size;
    if (!ReadVarint32(input, size))
        ythrow yexception() << "Stream is exhausted";

    TTempBufHelper buf(size);
    ::LoadPodArray(input, buf.Data(), size);
    CodedInputStream decoder(buf.Data(), size);
    decoder.SetTotalBytesLimit(MaxSizeBytes, WarningSizeBytes);
    if (!msg.ParseFromCodedStream(&decoder))
        ythrow yexception() << "Cannot read protobuf::Message (" << msg.GetTypeName() << ") from input stream";

}

TProtoReader::TProtoReader(TInputStream* input, const size_t bufferSize)
    : IStream(input)
    , Buffer(bufferSize)
{
}


bool TProtoReader::Load(Message& msg) {
    ui32 size;
    if (!ReadVarint32(IStream, size))
        return false;

    Buffer.Reserve(size);

    ::LoadPodArray(IStream, Buffer.Data(), size);
    CodedInputStream decoder((const ui8*)Buffer.Data(), size);
    if (!msg.ParseFromCodedStream(&decoder))
        ythrow yexception() << "Cannot read protobuf::Message from input stream";

    return true;
}

}
}
}