/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Copyright (c) 2015 Steven Lovegrove
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pangolin/video/drivers/pleora.h>

#ifdef DEBUGPLEORA
  #include <pangolin/utils/timer.h>
  #define TSTART() pangolin::basetime start,last,now; start = pangolin::TimeNow(); last = start;
  #define TGRABANDPRINT(...)  now = pangolin::TimeNow(); fprintf(stderr,"  PLEORA: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, " %fms.\n",1000*pangolin::TimeDiff_s(last, now)); last = now;
  #define DBGPRINT(...) fprintf(stderr,"  PLEORA: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr,"\n");
#else
  #define TSTART()
  #define TGRABANDPRINT(...)
  #define DBGPRINT(...)
#endif

namespace pangolin
{

inline void ThrowOnFailure(const PvResult& res)
{
    if(res.IsFailure()) {
        throw std::runtime_error("Failure: " + std::string(res.GetCodeString().GetAscii()) );
    }
}

template<typename T>
struct PleoraParamTraits;

template<> struct PleoraParamTraits<bool> {
    typedef PvGenBoolean PvType;
};
template<> struct PleoraParamTraits<int64_t> {
    typedef PvGenInteger PvType;
};
template<> struct PleoraParamTraits<float> {
    typedef PvGenFloat PvType;
};
template<> struct PleoraParamTraits<std::string> {
    typedef PvGenString PvType;
};

template<typename T>
T GetParam(PvGenParameterArray* params, const char* name)
{
    typedef typename PleoraParamTraits<T>::PvType PvType;
    PvType* param = dynamic_cast<PvType*>( params->Get(name) );
    if(!param) {
        throw std::runtime_error("Incorrect type");
    }
    T ret;
    PvResult res = param->GetValue(ret);
    if(res.IsFailure()) {
        throw std::runtime_error("Cannot get value: " + std::string(res.GetCodeString().GetAscii()) );
    }
    return ret;
}

template<typename T>
bool SetParam(PvGenParameterArray* params, const char* name, T val)
{
    typedef typename PleoraParamTraits<T>::PvType PvType;
    PvType* param = dynamic_cast<PvType*>( params->Get(name) );
    if(!param) {
        throw std::runtime_error("Unable to get parameter handle: " + std::string(name) );
    }

    if(!param->IsWritable()) {
        throw std::runtime_error("Cannot set value for " + std::string(name) );
    }

    PvResult res = param->SetValue(val);
    if(res.IsFailure()) {
        throw std::runtime_error("Cannot set value: " + std::string(res.GetCodeString().GetAscii()) );
    }
    return true;
}

inline const PvDeviceInfo* SelectDevice( PvSystem& aSystem, const char* model_name = 0, const char* serial_num = 0, size_t index = 0 )
{
    aSystem.Find();

    // Enumerate all devices, select first that matches criteria
    size_t matches = 0;
    for ( uint32_t i = 0; i < aSystem.GetInterfaceCount(); i++ ) {
        const PvInterface *lInterface = dynamic_cast<const PvInterface *>( aSystem.GetInterface( i ) );
        if ( lInterface ) {
            for ( uint32_t j = 0; j < lInterface->GetDeviceCount(); j++ ) {
                const PvDeviceInfo *lDI = dynamic_cast<const PvDeviceInfo *>( lInterface->GetDeviceInfo( j ) );
                if ( lDI && lDI->IsConfigurationValid() ) {
                    if( model_name && strcmp(lDI->GetModelName().GetAscii(), model_name) )
                        continue;
                    if( serial_num && strcmp(lDI->GetSerialNumber().GetAscii(), serial_num) )
                        continue;
                    if(matches == index) {
                        return lDI;
                    }
                    ++matches;
                }
            }
        }
    }

    return 0;
}

VideoPixelFormat PleoraFormat(const PvGenEnum* pfmt)
{
    std::string spfmt = pfmt->ToString().GetAscii();
    if( !spfmt.compare("Mono8") ) {
        return VideoFormatFromString("GRAY8");
    } else if( !spfmt.compare("Mono10p") ) {
        return VideoFormatFromString("GRAY10");
    } else if( !spfmt.compare("Mono12p") ) {
        return VideoFormatFromString("GRAY12");
    } else if( !spfmt.compare("Mono10") || !spfmt.compare("Mono12")) {
        return VideoFormatFromString("GRAY16LE");
    } else {
        throw VideoException("Unknown Pleora pixel format", spfmt);
    }
}

PleoraVideo::PleoraVideo(Params& p): size_bytes(0), lPvSystem(0), lDevice(0), lStream(0), lDeviceParams(0), lStart(0), lStop(0),
    lTemperatureCelcius(0), getTemp(false), lStreamParams(0), stand_alone_grab_thread(false), quit_grab_thread(true), validGrabbedBuffers(0)
{
    std::string sn;
    std::string mn;
    int index = 0;
    size_t buffer_count = PleoraVideo::DEFAULT_BUFFER_COUNT;
    Params device_params;

    for(Params::ParamMap::iterator it = p.params.begin(); it != p.params.end(); it++) {
        if(it->first == "model"){
            mn = it->second;
        } else if(it->first == "sn"){
            sn = it->second;
        } else if(it->first == "idx"){
            index = p.Get<int>("idx", 0);
        } else if(it->first == "buffers"){
            buffer_count = p.Get<size_t>("buffers", PleoraVideo::DEFAULT_BUFFER_COUNT);
        } else {
            device_params.Set(it->first, it->second);
        }
    }

    InitDevice(mn.empty() ? 0 : mn.c_str(), sn.empty() ? 0 : sn.c_str(), index);
    SetDeviceParams(device_params);
    InitStream();

    InitPangoStreams();
    InitBuffers(buffer_count);

    Start();
}


PleoraVideo::~PleoraVideo()
{
    Stop();
    DeinitBuffers();
    DeinitStream();
    DeinitDevice();
}

void PleoraVideo::InitDevice(
        const char* model_name, const char* serial_num, size_t index
) {
    lPvSystem = new PvSystem();
    if ( !lPvSystem ) {
        throw pangolin::VideoException("Pleora: Unable to create PvSystem");
    }

    lDeviceInfo = SelectDevice(*lPvSystem, model_name, serial_num, index);
    if ( !lDeviceInfo ) {
        delete lPvSystem;
        throw pangolin::VideoException("Pleora: Unable to select device");
    }

    PvResult lResult;
    lDevice = PvDevice::CreateAndConnect( lDeviceInfo, &lResult );
    if ( !lDevice ) {
        delete lPvSystem;
        throw pangolin::VideoException("Pleora: Unable to connect to device", lResult.GetDescription().GetAscii() );
    }

    lDeviceParams = lDevice->GetParameters();
}

void PleoraVideo::DeinitDevice()
{
    if(lDevice) {
        lDevice->Disconnect();
        PvDevice::Free( lDevice );
        lDevice = 0;
    }

    delete lPvSystem;
    lPvSystem = 0;
}

void PleoraVideo::InitStream()
{
    // Setup Stream
    PvResult lResult;
    lStream = PvStream::CreateAndOpen( lDeviceInfo->GetConnectionID(), &lResult );
    if ( !lStream ) {
        DeinitDevice();
        throw pangolin::VideoException("Pleora: Unable to open stream", lResult.GetDescription().GetAscii() );
    }
    lStreamParams = lStream->GetParameters();
}

void PleoraVideo::DeinitStream()
{
    if(lStream) {
        lStream->Close();
        PvStream::Free( lStream );
        lStream = 0;
    }
}


void PleoraVideo::SetDeviceParams(Params& p) {

    lStart = dynamic_cast<PvGenCommand*>( lDeviceParams->Get( "AcquisitionStart" ) );
    lStop = dynamic_cast<PvGenCommand*>( lDeviceParams->Get( "AcquisitionStop" ) );

    for(Params::ParamMap::iterator it = p.params.begin(); it != p.params.end(); it++) {
        if(it->first == "use_separate_thread"){
            stand_alone_grab_thread = p.Get<bool>("use_separate_thread",false);
        } else if(it->first == "get_temperature"){
            getTemp = p.Get<bool>("get_temperature",false);
        } else {
            try {
                PvGenParameter* par = lDeviceParams->Get(PvString(it->first.c_str()));
                if(par) {
                  PvResult r = par->FromString(PvString(it->second.c_str()));
                  if(!r.IsOK()){
                     pango_print_error("Error setting parameter %s to:%s Reason:%s\n", it->first.c_str(), it->second.c_str(), r.GetDescription().GetAscii());
                  } else {
                     pango_print_info("Setting parameter %s to:%s\n", it->first.c_str(), it->second.c_str());
                  }
                } else {
                  pango_print_error("Parameter %s not recognized\n", it->first.c_str());
                }
            } catch(std::runtime_error e) {
                pango_print_error("Set parameter %s: %s\n", it->first.c_str(), e.what());
            }
        }
    }

    // Get Handles to properties we'll be using.
    lAnalogGain = lDeviceParams->GetInteger("AnalogGain");
    lAnalogBlackLevel = lDeviceParams->GetInteger("AnalogBlackLevel");
    lExposure = lDeviceParams->GetFloat("ExposureTime");
    lAquisitionMode = lDeviceParams->GetEnum("AcquisitionMode");
    lTriggerSource = lDeviceParams->GetEnum("TriggerSource");
    lTriggerMode = lDeviceParams->GetEnum("TriggerMode");

    if(getTemp) {
        lTemperatureCelcius = lDeviceParams->GetFloat("DeviceTemperatureCelsius");
        pango_print_warn("Warning: get_temperature might add a blocking call taking several ms to each frame read.");
    }

}

void PleoraVideo::InitBuffers(size_t buffer_count)
{
    // Reading payload size from device
    const uint32_t lSize = lDevice->GetPayloadSize();

    // Use buffer_count or the maximum number of buffers, whichever is smaller
    const uint32_t lBufferCount = ( lStream->GetQueuedBufferMaximum() < buffer_count ) ?
        lStream->GetQueuedBufferMaximum() :
        buffer_count;

    // Allocate buffers and queue
    for( uint32_t i = 0; i < lBufferCount; i++ ) {
        PvBuffer *lBuffer = new PvBuffer;
        lBuffer->Alloc( static_cast<uint32_t>( lSize ) );
        lBufferList.push_back( lBuffer );
    }
}

void PleoraVideo::DeinitBuffers()
{
    // Free buffers
    for( BufferList::iterator lIt = lBufferList.begin(); lIt != lBufferList.end(); lIt++ ) {
        delete *lIt;
    }
    lBufferList.clear();
}

void PleoraVideo::InitPangoStreams()
{
    // Get actual width, height and payload size
    const int w = DeviceParam<int64_t>("Width");
    const int h = DeviceParam<int64_t>("Height");
    const uint32_t lSize = lDevice->GetPayloadSize();

    // Setup pangolin for stream
    PvGenEnum* lpixfmt = dynamic_cast<PvGenEnum*>( lDeviceParams->Get("PixelFormat") );
    const VideoPixelFormat fmt = PleoraFormat(lpixfmt);
    streams.push_back(StreamInfo(fmt, w, h, (w*fmt.bpp)/8));
    size_bytes = lSize;
}

unsigned int PleoraVideo::AvailableFrames() const
{
    return validGrabbedBuffers;
}

bool PleoraVideo::DropNFrames(uint32_t n)
{
    if(n > validGrabbedBuffers) return false;

    if(stand_alone_grab_thread) {
        grabbedBuffListMtx.lock();
        GrabbedBufferList::iterator lIt = lGrabbedBuffList.begin();
        while(n > 0) {
            //Mark old buffers as invalid so that they can be reclaimed.
            if(lIt->valid) {
                lIt->valid = false;
                --validGrabbedBuffers;
                --n;
                DBGPRINT("DropNFrames: marked 1 frame as invalid.")
            }
            ++lIt;
        }
        grabbedBuffListMtx.unlock();
    } else {
        while(n > 0) {
            lStream->QueueBuffer(lGrabbedBuffList.front().buff);
            lGrabbedBuffList.pop_front();
            --validGrabbedBuffers;
            --n;
            DBGPRINT("DropNFrames: removed 1 frame from the list and requeued it.")
        }
    }

    return true;
}

void PleoraVideo::Start()
{
    if(lStream->GetQueuedBufferCount() == 0) {
        // Queue all buffers in the stream
        for( BufferList::iterator lIt = lBufferList.begin(); lIt != lBufferList.end(); lIt++ ) {
            lStream->QueueBuffer( *lIt );
        }
        lDevice->StreamEnable();
        lStart->Execute();

        if(stand_alone_grab_thread) {
            quit_grab_thread = false;
            grab_thread = boostd::thread(boostd::ref(*this));
        }
    } else {
        pango_print_warn("PleoraVideo: Already started.\n");
    }
}

void PleoraVideo::Stop()
{
    // stop grab thread
    if(stand_alone_grab_thread) {
        quit_grab_thread = true;
        if(grab_thread.joinable()) {
           grab_thread.join();
        }
    }

    lStreamMtx.lock();
    if(lStream->GetQueuedBufferCount() > 0) {
        lStop->Execute();
        lDevice->StreamDisable();

        // Abort all buffers from the stream and dequeue
        lStream->AbortQueuedBuffers();
        while ( lStream->GetQueuedBufferCount() > 0 ) {
            PvBuffer *lBuffer = NULL;
            PvResult lOperationResult;
            lStream->RetrieveBuffer( &lBuffer, &lOperationResult );
        }
    } else {
        pango_print_warn("PleoraVideo: Already stopped.\n");
    }
    lStreamMtx.unlock();
}

size_t PleoraVideo::SizeBytes() const
{
    return size_bytes;
}

const std::vector<StreamInfo>& PleoraVideo::Streams() const
{
    return streams;
}

bool PleoraVideo::ParseBuffer(PvBuffer* lBuffer,  unsigned char* image)
{
  TSTART()
  if ( lBuffer->GetPayloadType() == PvPayloadTypeImage ) {
      PvImage *lImage = lBuffer->GetImage();
      TGRABANDPRINT("GetImage took ")
      std::memcpy(image, lImage->GetDataPointer(), size_bytes);
      TGRABANDPRINT("memcpy took ")
      // Required frame properties
      frame_properties[PANGO_CAPTURE_TIME_US] = json::value(lBuffer->GetTimestamp());
      frame_properties[PANGO_HOST_RECEPTION_TIME_US] = json::value(lBuffer->GetReceptionTime());
      TGRABANDPRINT("Frame properties took ")

      // Optional frame properties
      if(lTemperatureCelcius != 0) {
          double val;
          PvResult lResult = lTemperatureCelcius->GetValue(val);
          if(lResult.IsSuccess()) {
              frame_properties[PANGO_SENSOR_TEMPERATURE_C] = json::value(val);
          } else {
              pango_print_error("DeviceTemperatureCelsius %f fail\n", val);
          }
      }
      TGRABANDPRINT("GetTemperature took ")
      return true;
  } else {
      return false;
  }

}

void PleoraVideo::operator()()
{
    PvResult lResult;
    PvBuffer *lBuffer = NULL;
    PvResult lOperationResult;

    DBGPRINT("GRABTHREAD: Started.")
    while(!quit_grab_thread) {
        grabbedBuffListMtx.lock();
        // housekeeping
        GrabbedBufferList::iterator lIt =  lGrabbedBuffList.begin();
        while(lIt != lGrabbedBuffList.end()) {
            if(!lIt->valid) {
                lStreamMtx.lock();
                lStream->QueueBuffer(lIt->buff);
                lStreamMtx.unlock();
                lIt = lGrabbedBuffList.erase(lIt);
                DBGPRINT("GRABTHREAD: Requeued buffer.")
            } else {
                ++lIt;
            }
        }
        grabbedBuffListMtx.unlock();

        // Retrieve next buffer
        lStreamMtx.lock();
        lResult = lStream->RetrieveBuffer( &lBuffer, &lOperationResult, 50000);
        lStreamMtx.unlock();

        if ( !lResult.IsOK() ) {
            if(lResult && (lResult.GetCode() == PvResult::Code::NO_MORE_ITEM)) {
                // No more buffer left in the queue, wait a bit before retrying.
                boostd::this_thread::sleep_for(boostd::chrono::milliseconds(5));
            } else if(lResult && !(lResult.GetCode() == PvResult::Code::TIMEOUT)) {
                pango_print_warn("Pleora error: %s,\n", lResult.GetCodeString().GetAscii());
            }
        } else {
            grabbedBuffListMtx.lock();
            lGrabbedBuffList.push_back(GrabbedBuffer(lBuffer,lOperationResult,true));
            ++validGrabbedBuffers;
            grabbedBuffListMtx.unlock();
            cv.notify_all();
        }
        boostd::this_thread::yield();
    }

    grabbedBuffListMtx.lock();
    lStreamMtx.lock();
    // housekeeping
    GrabbedBufferList::iterator lIt =  lGrabbedBuffList.begin();
    while(lIt != lGrabbedBuffList.end()) {
        lStream->QueueBuffer(lIt->buff);
        lIt = lGrabbedBuffList.erase(lIt);
    }
    validGrabbedBuffers = 0;
    lStreamMtx.unlock();
    grabbedBuffListMtx.unlock();

    DBGPRINT("GRABTHREAD: Stopped.")

    return;
}

bool PleoraVideo::GrabNext( unsigned char* image, bool wait)
{
    const uint32_t timeout = wait ? 1000 : 0;
    bool good = false;
    TSTART()
    if(stand_alone_grab_thread) {
        DBGPRINT("GrabNext stand alone thread:")
        if((validGrabbedBuffers==0) && !wait) {
            DBGPRINT("Empty buffer list.")
            return false;
        }
        if((validGrabbedBuffers==0) && wait) {
            std::unique_lock<std::mutex> lk(cv_m);
            if(cv.wait_for(lk, boostd::chrono::seconds(5)) == boostd::cv_status::timeout)
                throw std::runtime_error("Pleora: blocking read for frames reached timeout.");
        }
        TGRABANDPRINT("Waiting for having at least 1 valid buffer took ")

        grabbedBuffListMtx.lock();
        DBGPRINT("%d frames valid, queue size %ld popping head",validGrabbedBuffers ,lGrabbedBuffList.size());
        GrabbedBufferList::iterator front = lGrabbedBuffList.begin();
        while(!front->valid) {
            ++front;
        }
        TGRABANDPRINT("Grabbing iterator to next frame (mtx lock) took ")

        if ( front->res.IsOK() ) {
            good = ParseBuffer(front->buff, image);
        }
        TGRABANDPRINT("ParseBuffer (good=%d) took ",good)

        // Flag frame as used, so that it will get released.
        front->valid = false;
        --validGrabbedBuffers;
        grabbedBuffListMtx.unlock();
        TGRABANDPRINT("Invalidating returned buffer took ")
    } else {
        DBGPRINT("GrabNext no thread:")

        RetriveAllAvailableBuffers((validGrabbedBuffers==0) ? timeout : 0);
        TGRABANDPRINT("Retriving all available buffers (valid frames in queue=%d, queue size=%ld) took ",validGrabbedBuffers ,lGrabbedBuffList.size())

        if(validGrabbedBuffers == 0) return false;

        // Retrieve next buffer from list and parse it
        GrabbedBufferList::iterator front = lGrabbedBuffList.begin();
        if ( front->res.IsOK() ) {
            good = ParseBuffer(front->buff, image);
        }
        TGRABANDPRINT("Parsing buffer took ")

        lStream->QueueBuffer(front->buff);
        TGRABANDPRINT("\tPLEORA:QueueBuffer: ")

        // Remove used buffer from list.
        lGrabbedBuffList.pop_front();
        --validGrabbedBuffers;
    }
    return good;
}


bool PleoraVideo::GrabNewest( unsigned char* image, bool wait )
{
    const uint32_t timeout = wait ? 0xFFFFFFFF : 0;
    bool good = false;

    TSTART()
    if(stand_alone_grab_thread) {
        DBGPRINT("GrabNewest stand alone thread:")
        grabbedBuffListMtx.lock();
        if(validGrabbedBuffers==0) {
            grabbedBuffListMtx.unlock();
            DBGPRINT("Empty buffer list, returning")
            return false;
        }
        DBGPRINT("(valid frames in queue=%d, queue size=%ld)",validGrabbedBuffers ,lGrabbedBuffList.size())

        GrabbedBufferList::iterator lItOneButLast = lGrabbedBuffList.end();
        --lItOneButLast;
        for(GrabbedBufferList::iterator lIt = lGrabbedBuffList.begin(); lIt != lItOneButLast; ++lIt) {
            //Mark old buffers as invalid so that they can be reclaimed.
            if(lIt->valid) {
                lIt->valid = false;
                --validGrabbedBuffers;
                DBGPRINT("marked 1 frame as invalid")
            }
        }
        TGRABANDPRINT("Flagging old frames as invalid took ")

        GrabbedBufferList::iterator newest = --(lGrabbedBuffList.end());
        if ( newest->res.IsOK() ) {
            good = ParseBuffer(newest->buff, image);
        }
        TGRABANDPRINT("Parsing buffer took ")

        // Flag frame as used, so that it will get released.
        newest->valid = false;
        --validGrabbedBuffers;
        grabbedBuffListMtx.unlock();
        TGRABANDPRINT("Invalidating returned buffer took ")
    } else {
        DBGPRINT("GrabNewest no thread:")
        RetriveAllAvailableBuffers((validGrabbedBuffers==0) ? timeout : 0);
        TGRABANDPRINT("Retriving all available buffers (valid frames in queue=%d, queue size=%ld) took ",validGrabbedBuffers ,lGrabbedBuffList.size())

        if(validGrabbedBuffers == 0) {
            DBGPRINT("No valid buffers, returning.")
            return false;
        }
        if(validGrabbedBuffers > 1) DropNFrames(validGrabbedBuffers-1);
        TGRABANDPRINT("Dropping %d frames took ", (validGrabbedBuffers-1))

        // Retrieve next buffer from list and parse it
        GrabbedBufferList::iterator front = lGrabbedBuffList.begin();
        if ( front->res.IsOK() ) {
            good = ParseBuffer(front->buff, image);
        }
        TGRABANDPRINT("Parsing buffer took ")

        lStream->QueueBuffer(front->buff);
        TGRABANDPRINT("Requeueing buffer took ")

        // Remove used buffer from list.
        lGrabbedBuffList.pop_front();
        --validGrabbedBuffers;
    }
    return good;
}

void PleoraVideo::RetriveAllAvailableBuffers(uint32_t timeout){
    PvBuffer *lBuffer = NULL;
    PvResult lOperationResult;
    PvResult lResult;
    TSTART()
    do {
        lResult = lStream->RetrieveBuffer( &lBuffer, &lOperationResult, timeout);
        if ( !lResult.IsOK() ) {
            if(lResult && !(lResult.GetCode() == PvResult::Code::TIMEOUT)) {
                pango_print_warn("Pleora error: %s,\n'%s'\n", lResult.GetCodeString().GetAscii(), lResult.GetDescription().GetAscii() );
            }
            return;
        } else if( !lOperationResult.IsOK() ) {
            pango_print_warn("Pleora error %s,\n'%s'\n", lOperationResult.GetCodeString().GetAscii(), lResult.GetDescription().GetAscii() );
            lStream->QueueBuffer( lBuffer );
            return;
        }
        lGrabbedBuffList.push_back(GrabbedBuffer(lBuffer,lOperationResult,true));
        ++validGrabbedBuffers;
        TGRABANDPRINT("Attempt retrieving buffer (timeout=%d validbuffer=%d) took ", timeout, validGrabbedBuffers)
        timeout = 0;
    } while (lResult.IsOK());
}

int64_t PleoraVideo::GetGain()
{
    int64_t val;
    if(lAnalogGain) {
        ThrowOnFailure( lAnalogGain->GetValue(val) );
    }
    return val;
}

void PleoraVideo::SetGain(int64_t val)
{
    if(val >= 0 && lAnalogGain && lAnalogGain->IsWritable()) {
        ThrowOnFailure( lAnalogGain->SetValue(val) );
        frame_properties[PANGO_ANALOG_GAIN] = json::value(val);
    }
}

int64_t PleoraVideo::GetAnalogBlackLevel()
{
    int64_t val;
    if(lAnalogGain) {
        ThrowOnFailure( lAnalogBlackLevel->GetValue(val) );
    }
    return val;
}

void PleoraVideo::SetAnalogBlackLevel(int64_t val)
{
    if(val >= 0 && lAnalogBlackLevel&& lAnalogBlackLevel->IsWritable()) {
        ThrowOnFailure( lAnalogBlackLevel->SetValue(val) );
        frame_properties[PANGO_ANALOG_BLACK_LEVEL] = json::value(val);
    }
}

double PleoraVideo::GetExposure()
{
    double val;
    if( lExposure ) {
        ThrowOnFailure( lExposure->GetValue(val));
    }
    return val;
}

void PleoraVideo::SetExposure(double val)
{
    if(val > 0 && lExposure && lExposure->IsWritable() ) {
        ThrowOnFailure( lExposure->SetValue(val) );
        frame_properties[PANGO_EXPOSURE_US] = json::value(val);
    }
}

//use 0,0,1 for line0 hardware trigger.
//use 2,252,0 for software continuous
void PleoraVideo::SetupTrigger(bool triggerActive, int64_t triggerSource, int64_t acquisitionMode)
{
    if(lAquisitionMode && lTriggerSource && lTriggerMode &&
        lAquisitionMode->IsWritable() && lTriggerSource->IsWritable() && lTriggerMode->IsWritable() ) {
        // Check input is valid.
        const PvGenEnumEntry* entry_src;
        const PvGenEnumEntry* entry_acq;
        lTriggerSource->GetEntryByValue(triggerSource, &entry_src);
        lAquisitionMode->GetEntryByValue(acquisitionMode, &entry_acq);

        if(entry_src && entry_acq) {
            ThrowOnFailure(lTriggerMode->SetValue(triggerActive ? 1 : 0));
            if(triggerActive) {
                pango_print_debug("Pleora: external trigger active\n");
                ThrowOnFailure(lTriggerSource->SetValue(triggerSource));
                ThrowOnFailure(lAquisitionMode->SetValue(acquisitionMode));
            }
        }else{
            pango_print_error("Bad values for trigger options.");
        }
    }
}

template<typename T>
T PleoraVideo::DeviceParam(const char* name)
{
    return GetParam<T>(lDeviceParams, name);
}

template<typename T>
bool PleoraVideo::SetDeviceParam(const char* name, T val)
{
    return SetParam<T>(lDeviceParams, name, val);
}

template<typename T>
T PleoraVideo::StreamParam(const char* name)
{
    return GetParam<T>(lStreamParams, name);
}

template<typename T>
bool PleoraVideo::SetStreamParam(const char* name, T val)
{
    return SetParam<T>(lStreamParams, name, val);
}

}

#undef TSTART
#undef TGRABANDPRINT
#undef DBGPRINT
