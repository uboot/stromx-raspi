
/* 
 *  Copyright 2014 Thomas Fidler
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "stromx/raspi/Config.h"
#include "stromx/raspi/Locale.h"
#include "stromx/raspi/RaspiStillCam.h"

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>

#include <stromx/cvsupport/Image.h>
#include <stromx/runtime/Data.h>
#include <stromx/runtime/DataContainer.h>
#include <stromx/runtime/DataProvider.h>
#include <stromx/runtime/EnumParameter.h>
#include <stromx/runtime/Id2DataPair.h>
#include <stromx/runtime/NumericParameter.h>
#include <stromx/runtime/OperatorException.h>
#include <stromx/runtime/ParameterGroup.h>
#include <stromx/runtime/Variant.h>

namespace
{
    static boost::mutex mutex;
    static boost::condition_variable_any cond;
    static const int MMAL_CAMERA_PREVIEW_PORT = 0;
    static const int MMAL_CAMERA_CAPTURE_PORT = 2;
    static const int MMAL_PREVIEW_INPUT_PORT = 0;
    
    static const int STILLS_FRAME_RATE_NUM = 0;
    static const int STILLS_FRAME_RATE_DEN = 1;
    
    static const int MAX_WIDTH = 2592;
    static const int MAX_HEIGHT = 1944;
    static const int MAX_PREVIEW_WIDTH = 1024;
    static const int MAX_PREVIEW_HEIGHT = 768;
    static const float AWB_GAIN_MIN = 0;
    static const float AWB_GAIN_MAX = 5;
    
    static const int DEFAULT_SHUTTER_SPEED = 100000;
    static const int DEFAULT_BRIGHTNESS = 50;
    
    class Image : public stromx::runtime::ImageWrapper
    {
        virtual void allocate(const unsigned int /*width*/, const unsigned int /*height*/,
                              const Image::PixelType /*pixelType*/)
        {
            throw stromx::runtime::NotImplemented();
        }
        
        static const std::string TYPE;
        static const std::string PACKAGE;
        static const stromx::runtime::Version VERSION;
        
        MMAL_BUFFER_HEADER_T* m_buffer;
        
    public:
        Image(MMAL_BUFFER_HEADER_T* buffer)
          : stromx::runtime::ImageWrapper(buffer->alloc_size, buffer->data)
          , m_buffer(buffer)
        {
            mmal_buffer_header_mem_lock(m_buffer);
        }
        
        ~Image()
        {
            mmal_buffer_header_mem_unlock(m_buffer);
            mmal_buffer_header_release(m_buffer);
        }
            
        const stromx::runtime::Version & version() const { return VERSION; }
        const std::string & type() const { return TYPE; }
        const std::string & package() const { return PACKAGE; }
        
        Data* clone() const
        {
            throw stromx::runtime::NotImplemented();
        }
    };
    
    const std::string Image::TYPE = "Image";
    const std::string Image::PACKAGE = STROMX_RASPI_PACKAGE_NAME;
    const stromx::runtime::Version Image::VERSION = stromx::runtime::Version(0, 1, 0);
}
        
namespace stromx
{
    namespace raspi
    {
        typedef boost::lock_guard<boost::mutex> lock_t;
        typedef boost::unique_lock<boost::mutex> unique_lock_t;
    
        struct PARAM_FLOAT_RECT_T
        {
            double x;
            double y;
            double w;
            double h;
        };

        const std::string RaspiStillCam::TYPE("RaspiStillCam");
        const std::string RaspiStillCam::PACKAGE(STROMX_RASPI_PACKAGE_NAME);
        const runtime::Version RaspiStillCam::VERSION(STROMX_RASPI_VERSION_MAJOR,STROMX_RASPI_VERSION_MINOR,STROMX_RASPI_VERSION_PATCH);

        RaspiStillCam::RaspiStillCam()
          : OperatorKernel(TYPE, PACKAGE, VERSION, setupInitParameters()),
            m_cameraComponent(0),
            m_previewComponent(0),
            m_previewConnection(0),
            m_outBufferPool(0),
            m_port(0),
            m_buffer(0),
            m_numBuffers(1),
            m_resolution(RESOLUTION_1280_BY_960),
            m_hasTriggerInput(false),
            m_awbGainRed(1.0),
            m_awbGainBlue(1.0)
        {
        }

        const std::vector<const runtime::Parameter*> RaspiStillCam::setupInitParameters()
        {
            using namespace runtime;
            
            std::vector<const Parameter*> parameters;

            Parameter* hasTriggerInput = new Parameter(HAS_TRIGGER_INPUT, Variant::BOOL);
            hasTriggerInput->setTitle(L_("Has trigger input"));
            hasTriggerInput->setAccessMode(Parameter::NONE_WRITE);
            parameters.push_back(hasTriggerInput);

            return parameters;
        }

        const std::vector<const runtime::Parameter*> RaspiStillCam::setupParameters()
        {
            using namespace runtime;
            
            std::vector<const Parameter*> parameters;

            Parameter* shutterSpeed = new NumericParameter<UInt32>(SHUTTER_SPEED);
            shutterSpeed->setTitle(L_("Shutter speed in microseconds"));
            shutterSpeed->setAccessMode(Parameter::ACTIVATED_WRITE);
            parameters.push_back(shutterSpeed);
            
            NumericParameter<UInt32>* numBuffers = new NumericParameter<UInt32>(NUM_BUFFERS);
            numBuffers->setTitle(L_("Number of buffers"));
            numBuffers->setAccessMode(Parameter::INITIALIZED_WRITE);
            numBuffers->setMin(UInt32(1));
            parameters.push_back(numBuffers);
            
            EnumParameter* resolution = new EnumParameter(RESOLUTION);
            resolution->setAccessMode(Parameter::INITIALIZED_WRITE);
            resolution->setTitle(L_("Resolution"));
            resolution->add(EnumDescription(Enum(RESOLUTION_2560_BY_1920), "2560 x 1920"));
            resolution->add(EnumDescription(Enum(RESOLUTION_1280_BY_960), "1280 x 960"));
            resolution->add(EnumDescription(Enum(RESOLUTION_640_BY_480), "640 x 480"));
            parameters.push_back(resolution);
            
            ParameterGroup* roiGroup = new ParameterGroup(ROI_GROUP);
            roiGroup->setTitle(L_("Region of interest"));
            parameters.push_back(roiGroup);
        
            NumericParameter<Float32>* width = new NumericParameter<Float32>(WIDTH, roiGroup);
            width->setTitle(L_("ROI width"));
            width->setAccessMode(Parameter::ACTIVATED_WRITE);
            width->setMin(Float32(0.0));
            width->setMax(Float32(1.0));
            parameters.push_back(width);
        
            NumericParameter<Float32>* height = new NumericParameter<Float32>(HEIGHT, roiGroup);
            height->setTitle(L_("ROI height"));
            height->setAccessMode(Parameter::ACTIVATED_WRITE);
            height->setMin(Float32(0.0));
            height->setMax(Float32(1.0));
            parameters.push_back(height);
            
            NumericParameter<Float32>* top = new NumericParameter<Float32>(TOP, roiGroup);
            top->setTitle(L_("ROI top offset"));
            top->setAccessMode(Parameter::ACTIVATED_WRITE);
            top->setMin(Float32(0.0));
            top->setMax(Float32(1.0));
            parameters.push_back(top);
            
            NumericParameter<Float32>* left = new NumericParameter<Float32>(LEFT, roiGroup);
            left->setTitle(L_("ROI left offset"));
            left->setAccessMode(Parameter::ACTIVATED_WRITE);
            left->setMin(Float32(0.0));
            left->setMax(Float32(1.0));
            parameters.push_back(left);
            
            EnumParameter* autoWhiteBalance = new runtime::EnumParameter(AWB_MODE);
            autoWhiteBalance->setTitle("AWB mode");
            autoWhiteBalance->setAccessMode(Parameter::ACTIVATED_WRITE);
            autoWhiteBalance->add(EnumDescription(Enum(MMAL_PARAM_AWBMODE_OFF), L_("Off")));
            autoWhiteBalance->add(EnumDescription(Enum(MMAL_PARAM_AWBMODE_AUTO), L_("Auto")));
            autoWhiteBalance->add(EnumDescription(Enum(MMAL_PARAM_AWBMODE_SUNLIGHT), L_("Sunlight")));
            autoWhiteBalance->add(EnumDescription(Enum(MMAL_PARAM_AWBMODE_CLOUDY), L_("Cloudy")));
            autoWhiteBalance->add(runtime::EnumDescription(Enum(MMAL_PARAM_AWBMODE_SHADE), L_("Shade")));
            parameters.push_back(autoWhiteBalance);
            
            ParameterGroup* awbGroup = new ParameterGroup(AWB_GAIN_GROUP);
            awbGroup->setTitle("White balance");
            parameters.push_back(awbGroup);
            
            NumericParameter<Float32>* awbRed = new NumericParameter<Float32>(AWB_GAIN_RED, awbGroup);
            awbRed->setTitle("AWB gain red");
            awbRed->setAccessMode(runtime::Parameter::ACTIVATED_WRITE);
            awbRed->setMin(Float32(AWB_GAIN_MIN));
            awbRed->setMax(Float32(AWB_GAIN_MAX));
            parameters.push_back(awbRed);
            
            NumericParameter<Float32>* awbBlue = new NumericParameter<Float32>(AWB_GAIN_BLUE, awbGroup);
            awbBlue->setTitle("AWB gain blue");
            awbBlue->setAccessMode(runtime::Parameter::ACTIVATED_WRITE);
            awbBlue->setMin(Float32(AWB_GAIN_MIN));
            awbBlue->setMax(Float32(AWB_GAIN_MAX));
            parameters.push_back(awbBlue);

            return parameters;
        }
        
        
        const std::vector< const runtime::Description* > RaspiStillCam::setupInputs()
        {
            std::vector<const runtime::Description*> inputs;
            
            if (m_hasTriggerInput)
            {
                runtime::Description* trigger = new runtime::Description(TRIGGER, runtime::Variant::TRIGGER);
                trigger->setTitle(L_("Trigger"));
                inputs.push_back(trigger);
            }
            
            return inputs;
        }

        const std::vector< const runtime::Description* > RaspiStillCam::setupOutputs()
        {
            std::vector<const runtime::Description*> outputs;

            runtime::Description* outputImage = new runtime::Description(IMAGE, runtime::Variant::IMAGE);
            outputImage->setTitle(L_("Output image"));
            outputs.push_back(outputImage);

            return outputs;
        }
        
        void RaspiStillCam::setParameter(unsigned int id, const runtime::Data& value)
        {
            MMAL_STATUS_T status;
            PARAM_FLOAT_RECT_T roi;
            MMAL_PARAMETER_AWBMODE_T awbMode;
            MMAL_PARAMETER_AWB_GAINS_T awbGains;
            float gain;
            
            try
            {
                switch(id)
                {
                case SHUTTER_SPEED:
                    status = mmal_port_parameter_set_uint32(m_cameraComponent->control,
                                                            MMAL_PARAMETER_SHUTTER_SPEED,
                                                            runtime::data_cast<runtime::UInt32>(value));
                    if(status != MMAL_SUCCESS)
                        throw runtime::ParameterError(parameter(id), *this);
                    break;
                case NUM_BUFFERS:
                {
                    runtime::UInt32 newNumBuffers = runtime::data_cast<runtime::UInt32>(value);
                    if(newNumBuffers < 1)
                        throw runtime::WrongParameterValue(parameter(NUM_BUFFERS), *this);
                    m_numBuffers = newNumBuffers;
                    break;
                }
                case RESOLUTION:
                    m_resolution = runtime::data_cast<runtime::Enum>(value);
                    break;
                case LEFT:
                    if(! getRoi(roi))
                        throw runtime::ParameterError(parameter(LEFT), *this);
                    roi.x = runtime::data_cast<runtime::Float32>(value);
                    if (! setRoi(roi))
                        throw runtime::WrongParameterValue(parameter(LEFT), *this);
                    setRoi(roi);
                    break;
                case TOP:
                    if(! getRoi(roi))
                        throw runtime::ParameterError(parameter(TOP), *this);
                    roi.y = runtime::data_cast<runtime::Float32>(value);
                    if (! setRoi(roi))
                        throw runtime::WrongParameterValue(parameter(TOP), *this);
                    setRoi(roi);
                    break;
                case WIDTH:
                    if (! getRoi(roi))
                        throw runtime::ParameterError(parameter(WIDTH), *this);
                    roi.w = runtime::data_cast<runtime::Float32>(value);
                    if (! setRoi(roi))
                        throw runtime::WrongParameterValue(parameter(WIDTH), *this);
                    break;
                case HEIGHT:
                    if (! getRoi(roi))
                        throw runtime::ParameterError(parameter(HEIGHT), *this);
                    roi.h = runtime::data_cast<runtime::Float32>(value);
                    if (! setRoi(roi))
                        throw runtime::WrongParameterValue(parameter(HEIGHT), *this);
                    break;
                case AWB_MODE:
                    awbMode = {
                        {MMAL_PARAMETER_AWB_MODE, sizeof(awbMode)},
                        MMAL_PARAM_AWBMODE_T(int(runtime::data_cast<runtime::Enum>(value)))
                    };
                    status = mmal_port_parameter_set(m_cameraComponent->control, &awbMode.hdr);
                    if(status != MMAL_SUCCESS)
                        throw runtime::ParameterError(parameter(id), *this);
                    break;
                case AWB_GAIN_RED:
                    gain = runtime::data_cast<runtime::Float32>(value);
                    awbGains = {
                        {MMAL_PARAMETER_CUSTOM_AWB_GAINS, sizeof(MMAL_PARAMETER_AWB_GAINS_T)},
                        {int32_t(gain * 65536), 65536}, 
                        {int32_t(float(m_awbGainBlue) * 65536), 65536}
                    };
                    status = mmal_port_parameter_set(m_cameraComponent->control, &awbGains.hdr);
                    if(status != MMAL_SUCCESS)
                        throw runtime::ParameterError(parameter(id), *this);
                    m_awbGainRed = gain;
                    break;
                case AWB_GAIN_BLUE:
                    gain = runtime::data_cast<runtime::Float32>(value);
                    awbGains = {
                        {MMAL_PARAMETER_CUSTOM_AWB_GAINS, sizeof(MMAL_PARAMETER_AWB_GAINS_T)},
                        {int32_t(float(m_awbGainRed) * 65536), 65536}, 
                        {int32_t(gain * 65536), 65536}
                    };
                    status = mmal_port_parameter_set(m_cameraComponent->control, &awbGains.hdr);
                    if(status != MMAL_SUCCESS)
                        throw runtime::ParameterError(parameter(id), *this);
                    m_awbGainBlue = gain;
                    break;
                case HAS_TRIGGER_INPUT:
                    m_hasTriggerInput = runtime::data_cast<runtime::Bool>(value);
                    break;
                default:
                    throw runtime::WrongParameterId(id,*this);
                }
            }
            catch(runtime::BadCast&)
            {
                throw runtime::WrongParameterType(parameter(id), *this);
            }
        }

        const runtime::DataRef RaspiStillCam::getParameter(const unsigned int id) const
        {
            MMAL_STATUS_T status;
            uint32_t value = 0;
            PARAM_FLOAT_RECT_T roi;
            MMAL_PARAMETER_AWBMODE_T awbMode;
            float gainRed, gainBlue;

            switch(id)
            {
            case SHUTTER_SPEED:
                status = mmal_port_parameter_get_uint32(m_cameraComponent->control,
                                                        MMAL_PARAMETER_SHUTTER_SPEED,
                                                        &value);
                if(status != MMAL_SUCCESS)
                    throw runtime::ParameterError(parameter(id), *this);
                
                return runtime::UInt32(value);
            case NUM_BUFFERS:
                return m_numBuffers;
            case RESOLUTION:
                return m_resolution;
            case LEFT:
                if(! getRoi(roi))
                    throw runtime::ParameterError(parameter(LEFT), *this);
                return runtime::Float32(roi.x);
            case TOP:
                if(! getRoi(roi))
                    throw runtime::ParameterError(parameter(TOP), *this);
                return runtime::Float32(roi.y);
            case WIDTH:
                if(! getRoi(roi))
                    throw runtime::ParameterError(parameter(WIDTH), *this);
                return runtime::Float32(roi.w);
            case HEIGHT:
                if(! getRoi(roi))
                    throw runtime::ParameterError(parameter(HEIGHT), *this);
                return runtime::Float32(roi.h);
            case AWB_MODE:
                awbMode = {
                    {MMAL_PARAMETER_AWB_MODE, sizeof(awbMode)},
                    MMAL_PARAM_AWBMODE_AUTO
                };
                status = mmal_port_parameter_get(m_cameraComponent->control, &awbMode.hdr);
                
                if(status != MMAL_SUCCESS)
                    throw runtime::ParameterError(parameter(id), *this);
                
                return runtime::Enum(awbMode.value);
            case AWB_GAIN_RED:
                return m_awbGainRed;
            case AWB_GAIN_BLUE:
                return m_awbGainBlue;
            case HAS_TRIGGER_INPUT:
                return m_hasTriggerInput;
            default:
                throw runtime::WrongParameterId(id,*this);
            }
        }
        
        void RaspiStillCam::initialize()
        {
            OperatorKernel::initialize(setupInputs(), setupOutputs(), setupParameters());
            
            bcm_host_init();

            MMAL_STATUS_T status;
            
            status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &m_cameraComponent);
            if(status != MMAL_SUCCESS)
            {
                deinitialize();
                throw runtime::OperatorError(*this, "Could not create mmal default camera.");
            }
            
            MMAL_PARAMETER_INT32_T camera_num = {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, 0};
            status = mmal_port_parameter_set(m_cameraComponent->control, &camera_num.hdr);
            if (status != MMAL_SUCCESS)
            {
                deinitialize();
                throw runtime::OperatorError(*this, "Could not select camera 0.");
            }
            
            if(!m_cameraComponent->output_num)
            {
                deinitialize();
                throw runtime::OperatorError(*this, "Default mmal camera has no outputs.");
            }

            status = mmal_port_enable(m_cameraComponent->control, 0);
            if(status != MMAL_SUCCESS)
            {
                deinitialize();
                throw runtime::OperatorError(*this, "Unable to enable control port.");
            }
            
            m_port = m_cameraComponent->output[MMAL_CAMERA_CAPTURE_PORT];
            m_port->userdata = reinterpret_cast<MMAL_PORT_USERDATA_T*>(this);

            {
                MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
                {
                    { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
                    /*.max_stills_w = */MAX_WIDTH,
                    /*.max_stills_h = */MAX_HEIGHT,
                    /*.stills_yuv422 = */0,
                    /*.one_shot_stills = */1,
                    /*.max_preview_video_w = */MAX_PREVIEW_WIDTH,
                    /*.max_preview_video_h = */MAX_PREVIEW_HEIGHT,
                    /*.num_preview_video_frames = */3,
                    /*.stills_capture_circular_buffer_height = */0,
                    /*.fast_preview_resume = */0,
                    /*.use_stc_timestamp = */MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
                };
                mmal_port_parameter_set(m_cameraComponent->control, &cam_config.hdr);
            }
            
            status = mmal_port_parameter_set_uint32(m_cameraComponent->control, MMAL_PARAMETER_SHUTTER_SPEED, DEFAULT_SHUTTER_SPEED);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to set default shutter speed.");
            }
            
            
            MMAL_PARAMETER_EXPOSUREMODE_T expMode = {
                {MMAL_PARAMETER_EXPOSURE_MODE, sizeof(expMode)}, 
                MMAL_PARAM_EXPOSUREMODE_T(MMAL_PARAM_EXPOSUREMODE_OFF)
            };
            status = mmal_port_parameter_set(m_cameraComponent->control, &expMode.hdr);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to set default exposure mode.");
            }
            
            MMAL_PARAMETER_AWBMODE_T awbMode = {
                {MMAL_PARAMETER_AWB_MODE, sizeof(awbMode)},
                MMAL_PARAM_AWBMODE_T(MMAL_PARAM_AWBMODE_OFF)
            };
            status = mmal_port_parameter_set(m_cameraComponent->control, &awbMode.hdr);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to set default AWB mode.");
            }
            
            MMAL_PARAMETER_AWB_GAINS_T awbGains = {
                {MMAL_PARAMETER_CUSTOM_AWB_GAINS, sizeof(awbGains)},
                {65536,65536}, {65536,65536}
            };
            status = mmal_port_parameter_set(m_cameraComponent->control, &awbGains.hdr);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to set default AWB gains.");
            }
            
            MMAL_RATIONAL_T value = {DEFAULT_BRIGHTNESS, 100};
            status = mmal_port_parameter_set_rational(m_cameraComponent->control, MMAL_PARAMETER_BRIGHTNESS, value);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to set brightness.");
            }
            
            status = mmal_component_create("vc.null_sink", &m_previewComponent);
            if (status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to create null sink component.");
            }
        }
        
        void RaspiStillCam::deinitialize()
        {           
            OperatorKernel::deinitialize();
            
            m_port = 0;
            mmal_component_destroy(m_cameraComponent);
            m_cameraComponent = 0;
            
            mmal_component_destroy(m_previewComponent);
            m_previewComponent = 0;
            
            bcm_host_deinit();
        }
        
        void RaspiStillCam::activate()
        {
            MMAL_STATUS_T status;
            MMAL_ES_FORMAT_T* format = m_port->format;
            
            // Set our stills format on the stills port
            format->encoding = MMAL_ENCODING_RGB24;
            format->encoding_variant = MMAL_ENCODING_RGB24;
            
            format->es->video.width = VCOS_ALIGN_UP(width(m_resolution), 32);
            format->es->video.height = VCOS_ALIGN_UP(height(m_resolution), 16);
            format->es->video.crop.x = 0;
            format->es->video.crop.y = 0;
            format->es->video.crop.width = width(m_resolution);
            format->es->video.crop.height = height(m_resolution);
            format->es->video.frame_rate.num = STILLS_FRAME_RATE_NUM;
            format->es->video.frame_rate.den = STILLS_FRAME_RATE_DEN;
            
            if (m_port->buffer_size < m_port->buffer_size_min)
              m_port->buffer_size = m_port->buffer_size_min;
            m_port->buffer_num = static_cast<uint32_t>(m_numBuffers);
            
            status = mmal_port_format_commit(m_port);
            if (status != MMAL_SUCCESS )
            {
                deactivate();
                throw runtime::OperatorError(*this, "Camera still format couldn't be set");
            }

            // Enable camera component
            status = mmal_component_enable(m_cameraComponent);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Could not enable camera.");
            }

            // Create output buffer pool
            m_outBufferPool = mmal_port_pool_create(m_port, m_port->buffer_num, m_port->buffer_size_recommended);
            if(m_outBufferPool == 0)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Could not create output buffer pool.");
            }
            
            // enable the still port
            status = mmal_port_enable(m_port, callbackOutVideoPort);
            if(status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to enable still port.");
            }
            
            status = mmal_component_enable(m_previewComponent);
            if (status != MMAL_SUCCESS)
            {
                deinitialize();
                throw runtime::OperatorError(*this, "Unable to enable preview component.");
            }
            
            MMAL_PORT_T* previewOutputPort = m_cameraComponent->output[MMAL_CAMERA_PREVIEW_PORT];
            MMAL_PORT_T* previewInputPort = m_previewComponent->input[MMAL_PREVIEW_INPUT_PORT];
            status =  mmal_connection_create(&m_previewConnection, previewOutputPort, previewInputPort, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);
            if (status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to connect MMAL preview ports.");
            }
            
            status =  mmal_connection_enable(m_previewConnection);
            if (status != MMAL_SUCCESS)
            {
                deactivate();
                throw runtime::OperatorError(*this, "Unable to enable preview connection.");
            }
        }

        void RaspiStillCam::deactivate()
        {
            // destroy connection
            mmal_connection_destroy(m_previewConnection);
            m_previewConnection = 0;
            
            // stop capture
            mmal_port_parameter_set_boolean(m_port,MMAL_PARAMETER_CAPTURE, 0);
            
            // disable processing on port
            mmal_port_disable(m_port);

            // Deactivate components
            mmal_component_disable(m_previewComponent);
            mmal_component_disable(m_cameraComponent);

            // Destroy buffers
            mmal_port_pool_destroy(m_port, m_outBufferPool);
            m_outBufferPool = 0;  
        }

        void RaspiStillCam::execute(runtime::DataProvider& provider)
        {
            if (m_hasTriggerInput)
            {
                runtime::Id2DataPair triggerMap(TRIGGER);
                provider.receiveInputData(triggerMap);
            }
            
            MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(m_outBufferPool->queue);
            
            if (!buffer)
                throw runtime::OperatorError(*this, "Unable to get a buffer from pool queue.");
            
            if (mmal_port_send_buffer(m_port, buffer)!= MMAL_SUCCESS)
                throw runtime::OperatorError(*this, "Unable to send a buffer to camera output port.");
            
            // start capture
            if (mmal_port_parameter_set_boolean(m_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS)
                throw runtime::OperatorError(*this, "Failed to start capture.");
            
            Image* image = 0;
            try
            {
                unique_lock_t lock(mutex);
                provider.unlockParameters();

                while(m_buffer == 0)
                    cond.wait(lock);

                provider.lockParameters();
                
                BOOST_ASSERT(m_buffer);
                image = new Image(m_buffer);
                image->initializeImage(width(m_resolution), height(m_resolution),
                                       width(m_resolution) * 3,
                                       m_buffer->data, runtime::Image::BGR_24);
                m_buffer = 0;
            }
            catch(boost::thread_interrupted&)
            {
                provider.lockParameters();
                throw runtime::Interrupt();
            }

            runtime::DataContainer outContainer = runtime::DataContainer(image);
            runtime::Id2DataPair outImageMapper(IMAGE, outContainer);
            provider.sendOutputData(outImageMapper);
        }

        void RaspiStillCam::callbackOutVideoPort(MMAL_PORT_T* port, MMAL_BUFFER_HEADER_T* buffer)
        {
            lock_t l(mutex);
            
            RaspiStillCam* cam = reinterpret_cast<RaspiStillCam*>(port->userdata);
            BOOST_ASSERT(! cam->m_buffer);
            cam->m_buffer = buffer;
            cond.notify_all();
        }
        
        int RaspiStillCam::width(const runtime::Enum & resolution)
        {
            switch(int(resolution))
            {
            case RESOLUTION_2560_BY_1920:
                return 2560;
            case RESOLUTION_1280_BY_960:
                return 1280;
            case RESOLUTION_640_BY_480:
                return 640;
            default:
                throw runtime::WrongArgument("Unknown camera resolution.");
            }
        }
        
        int RaspiStillCam::height(const runtime::Enum & resolution)
        {
            switch(int(resolution))
            {
            case RESOLUTION_2560_BY_1920:
                return 1920;
            case RESOLUTION_1280_BY_960:
                return 960;
            case RESOLUTION_640_BY_480:
                return 480;
            default:
                throw runtime::WrongArgument("Unknown camera resolution.");
            }
        }
        
        bool RaspiStillCam::getRoi(PARAM_FLOAT_RECT_T & rect) const
        {
            MMAL_PARAMETER_INPUT_CROP_T crop = {{MMAL_PARAMETER_INPUT_CROP, sizeof(MMAL_PARAMETER_INPUT_CROP_T)}};
            MMAL_STATUS_T status = mmal_port_parameter_get(m_cameraComponent->control, &crop.hdr);
            if(status != MMAL_SUCCESS)
                return false;
            
            rect.x = crop.rect.x / 65536.0;
            rect.y = crop.rect.y / 65536.0;
            rect.w = crop.rect.width / 65536.0;
            rect.h = crop.rect.height / 65536.0;
            
            return true;
        }
        
        bool RaspiStillCam::setRoi(const PARAM_FLOAT_RECT_T & rect)
        {
            MMAL_PARAMETER_INPUT_CROP_T crop = {{MMAL_PARAMETER_INPUT_CROP, sizeof(MMAL_PARAMETER_INPUT_CROP_T)}};
            
            crop.rect.x = rect.x * 65536;
            crop.rect.y = rect.y * 65536;
            crop.rect.width = rect.w * 65536;
            crop.rect.height = rect.h * 65536;
            
            MMAL_STATUS_T status = mmal_port_parameter_set(m_cameraComponent->control, &crop.hdr);
            if(status != MMAL_SUCCESS)
                return false;
            
            return true;
        }
    }
}
