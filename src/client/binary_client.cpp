/// @author Alexander Rykovanov 2012
/// @email rykovanov.as@gmail.com
/// @brief Remote server implementaion.
/// @license GNU LGPL
///
/// Distributed under the GNU LGPL License
/// (See accompanying file LICENSE or copy at
/// http://www.gnu.org/licenses/lgpl.html)
///

#include <opc/ua/protocol/utils.h>
#include <opc/ua/client/binary_client.h>
#include <opc/ua/client/remote_connection.h>

#include <opc/common/uri_facade.h>
#include <opc/ua/protocol/binary/stream.h>
#include <opc/ua/protocol/channel.h>
#include <opc/ua/protocol/secure_channel.h>
#include <opc/ua/protocol/session.h>
#include <opc/ua/protocol/string_utils.h>
#include <opc/ua/services/services.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <iostream>


namespace
{

    using namespace OpcUa;
    using namespace OpcUa::Binary;

    typedef std::map<uint32_t, std::function<void (PublishResult)>> SubscriptionCallbackMap;

    class BufferInputChannel : public OpcUa::InputChannel
    {
    public:
      BufferInputChannel(const std::vector<char> & buffer)
        : Buffer(buffer)
        , Pos(0)
      {
        Reset();
      }

      virtual std::size_t Receive(char * data, std::size_t size)
      {
        if (Pos >= Buffer.size())
          {
            return 0;
          }

        size = std::min(size, Buffer.size() - Pos);
        std::vector<char>::const_iterator begin = Buffer.begin() + Pos;
        std::vector<char>::const_iterator end = begin + size;
        std::copy(begin, end, data);
        Pos += size;
        return size;
      }

      void Reset()
      {
        Pos = 0;
      }

      virtual void Stop()
      {
      }

    //private:
      const std::vector<char> & Buffer;
      std::size_t Pos;
    };


    template <typename T>
    class RequestCallback
    {
    public:
      RequestCallback(const Common::Logger::SharedPtr & logger)
        : Logger(logger)
        , lock(m)
      {
      }

      void OnData(std::vector<char> data, ResponseHeader h)
      {
        //std::cout << ToHexDump(data);
        //std::realloc()
        Data = data; // std::move(data);
        //data.erase(data.begin(), data.end());
        this->header = h;
        doneEvent.notify_all();
      }

      T WaitForData(std::chrono::milliseconds msec)
      {
          T result;

        if (doneEvent.wait_for(lock, msec) == std::cv_status::timeout)
        { 
            LOG_CRITICAL(Logger, "{:90}| -->| Response timed out {}", FUNCTION_LINE_NAME, msec);
            //throw std::runtime_error(FUNCTION_LINE_NAME + "| Response timed out");
            return result;
        }

        result.Header = std::move(this->header);

        if (Data.empty())
          {
            //LOG_DEBUG
            LOG_WARN(Logger, "{:90}| received empty packet from server", FUNCTION_LINE_NAME);
          }
        else
          {
            BufferInputChannel bufferInput(Data);
            IStreamBinary in(bufferInput);
            in >> result;
          }

        return result;
      }

    //private:
      Common::Logger::SharedPtr Logger;
      std::vector<char> Data;
      ResponseHeader header;
      std::mutex m;
      std::unique_lock<std::mutex> lock;
      std::condition_variable doneEvent;
    };

    class CallbackThread
    {
    public:
        CallbackThread(const Common::Logger::SharedPtr& logger = nullptr)
            : Logger(logger)
            , StopRequest(false)
        {

        }

        void post(std::function<void()> callback)
        {
            LOG_DEBUG(Logger, "{:90}| CallbackThread: post -->", FUNCTION_LINE_NAME);

            std::unique_lock<std::mutex> lock(Mutex);
            Queue.push(callback);
            Condition.notify_one();

            LOG_DEBUG(Logger, "{:90}| CallbackThread: post <--", FUNCTION_LINE_NAME);
        }

        void Run()
        {
            while(true)
            {
                LOG_DEBUG(Logger, "{:90}| CallbackThread: waiting for next post", FUNCTION_LINE_NAME);

                std::unique_lock<std::mutex> lock(Mutex);
                Condition.wait(lock, [&]() { return (StopRequest == true) || (!Queue.empty()); });

                if(StopRequest)
                {
                    LOG_DEBUG(Logger, "{:90}| CallbackThread: exited", FUNCTION_LINE_NAME);

                    return;
                }

                while(!Queue.empty())   //to avoid crashing on spurious events
                {
                    std::function<void()> callback = Queue.front();
                    Queue.pop();
                    lock.unlock();
                    LOG_DEBUG(Logger, "{:90}| -->CallbackThread: calling callback", FUNCTION_LINE_NAME);
                    callback();
                    LOG_DEBUG(Logger, "{:90}| <--CallbackThread: callback finished", FUNCTION_LINE_NAME);

                    lock.lock();
                }
            }
        }

        void Stop()
        {
            LOG_DEBUG(Logger, "{:90}| CallbackThread: stopping", FUNCTION_LINE_NAME);

            StopRequest = true;
            Condition.notify_all();
        }

      //private:
        Common::Logger::SharedPtr Logger;
        std::mutex Mutex;
        std::condition_variable Condition;
        std::atomic<bool> StopRequest;
        std::queue<std::function<void()>> Queue;
    };

    class BinaryClient
        : public Services
        , public AttributeServices
        , public EndpointServices
        , public MethodServices
        , public NodeManagementServices
        , public SubscriptionServices
        , public ViewServices
        , public std::enable_shared_from_this<BinaryClient>
    {
    //private:
        typedef std::function<void(std::vector<char>, ResponseHeader)> ResponseCallback;
        typedef std::map<uint32_t, ResponseCallback> CallbackMap;
        std::vector<char> messageBuffer;

    public:
        BinaryClient(std::shared_ptr<IOChannel> channel, const SecureConnectionParams& params, const Common::Logger::SharedPtr& logger)
            : Channel(channel)
            , Stream(channel)
            , Params(params)
            , SequenceNumber(1)
            , RequestNumber(1)
            , RequestHandle(0)
            , Logger(logger)
            , CallbackService(logger)

        {
          //Initialize the worker thread for subscriptions
            callback_thread = std::thread([&]() { CallbackService.Run(); });
            try
            {
                HelloServer(params);
            }
            catch(...)
            {
                CallbackService.Stop();
                callback_thread.join();
                throw;
            }

            ReceiveThread = std::thread([this]()
            {
                try
                {
                    while(!Finished)
                    {
                        Receive();
                    }
                }

                catch(const std::exception& exc)
                {
                    if(Finished) { return; }

                    LOG_ERROR(Logger, "{:90}| ReceiveThread: error receiving data: {}", FUNCTION_LINE_NAME, exc.what());
                }
            });
        }

        ~BinaryClient()
        {
            Finished = true;

            LOG_DEBUG(Logger, "{:90}| stopping callback thread", FUNCTION_LINE_NAME);

            CallbackService.Stop();

            LOG_DEBUG(Logger, "{:90}| joining service thread", FUNCTION_LINE_NAME);

            callback_thread.join(); //Not sure it is necessary

            Channel->Stop();

            LOG_DEBUG(Logger, "{:90}| joining receive thread", FUNCTION_LINE_NAME);

            ReceiveThread.join();

            LOG_DEBUG(Logger, "{:90}| receive thread stopped", FUNCTION_LINE_NAME);
        }

        ////////////////////////////////////////////////////////////////
        /// Session Services
        ////////////////////////////////////////////////////////////////
        virtual CreateSessionResponse CreateSession(const RemoteSessionParameters& parameters) override
        {
            LOG_DEBUG(Logger, "{:90}| CreateSession -->", FUNCTION_LINE_NAME);

            CreateSessionRequest request;
            request.Header = CreateRequestHeader();

            request.Parameters.ClientDescription.ApplicationUri = parameters.ClientDescription.ApplicationUri;
            request.Parameters.ClientDescription.ProductUri = parameters.ClientDescription.ProductUri;
            request.Parameters.ClientDescription.ApplicationName = parameters.ClientDescription.ApplicationName;
            request.Parameters.ClientDescription.ApplicationType = parameters.ClientDescription.ApplicationType;
            request.Parameters.ClientDescription.GatewayServerUri = parameters.ClientDescription.GatewayServerUri;
            request.Parameters.ClientDescription.DiscoveryProfileUri = parameters.ClientDescription.DiscoveryProfileUri;
            request.Parameters.ClientDescription.DiscoveryUrls = parameters.ClientDescription.DiscoveryUrls;

            request.Parameters.ServerUri = parameters.ServerURI;
            request.Parameters.EndpointUrl = parameters.EndpointUrl; // TODO make just endpoint.URL;
            request.Parameters.SessionName = parameters.SessionName;
            request.Parameters.ClientNonce = ByteString(std::vector<uint8_t>(32, 0));
            request.Parameters.ClientCertificate = ByteString(parameters.ClientCertificate);
            request.Parameters.RequestedSessionTimeout = parameters.Timeout;
            request.Parameters.MaxResponseMessageSize = 65536;
            CreateSessionResponse response = Send<CreateSessionResponse>(request);
            AuthenticationToken = response.Parameters.AuthenticationToken;

            LOG_DEBUG(Logger, "{:90}| CreateSession <--", FUNCTION_LINE_NAME);

            return response;
        }

        ActivateSessionResponse ActivateSession(const ActivateSessionParameters& session_parameters) override
        {
            LOG_DEBUG(Logger, "{:90}| ActivateSession -->", FUNCTION_LINE_NAME);

            ActivateSessionRequest request;
            request.Parameters = session_parameters;
            request.Parameters.LocaleIds.push_back("en");
            ActivateSessionResponse response = Send<ActivateSessionResponse>(request);

            LOG_DEBUG(Logger, "{:90}| ActivateSession <--", FUNCTION_LINE_NAME);

            return response;
        }

        virtual CloseSessionResponse CloseSession() override
        {
            LOG_DEBUG(Logger, "{:90}| CloseSession -->", FUNCTION_LINE_NAME);

            CloseSessionRequest request;
            CloseSessionResponse response = Send<CloseSessionResponse>(request);
            RemoveSelfReferences();

            LOG_DEBUG(Logger, "{:90}| CloseSession <--", FUNCTION_LINE_NAME);

            return response;
        }

        virtual void AbortSession() override
        {
            LOG_DEBUG(Logger, "{:90}| AbortSession -->", FUNCTION_LINE_NAME);

            RemoveSelfReferences();

            LOG_DEBUG(Logger, "{:90}| AbortSession <--", FUNCTION_LINE_NAME);
        }

        DeleteNodesResponse DeleteNodes(const std::vector<OpcUa::DeleteNodesItem>& nodesToDelete) override
        {
            LOG_DEBUG(Logger, "{:90}| DeleteNodes -->", FUNCTION_LINE_NAME);

            DeleteNodesRequest request;
            request.NodesToDelete = nodesToDelete;
            DeleteNodesResponse response = Send<DeleteNodesResponse>(request);

            LOG_DEBUG(Logger, "{:90}| DeleteNodes <--", FUNCTION_LINE_NAME);

            return response;
        }

        ////////////////////////////////////////////////////////////////
        /// Attribute Services
        ////////////////////////////////////////////////////////////////
        virtual std::shared_ptr<AttributeServices> Attributes() override
        {
            return shared_from_this();
        }

    public:
        virtual std::vector<DataValue> Read(const ReadParameters& params) const override
        {
            LOG_DEBUG(Logger, "{:90}| Read -->", FUNCTION_LINE_NAME);
            if(Logger && Logger->should_log(spdlog::level::trace))
            {
                for(ReadValueId attr : params.AttributesToRead)
                {
                    Logger->trace("{:90}| Read: node id: {} attr id: {}", FUNCTION_LINE_NAME, attr.NodeId, ToString(attr.AttributeId));
                }
            }

            ReadRequest request;
            request.Parameters = params;
            const ReadResponse response = Send<ReadResponse>(request);

            LOG_DEBUG(Logger, "{:90}| Read <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        virtual std::vector<OpcUa::StatusCode> Write(const std::vector<WriteValue>& values) override
        {
            LOG_DEBUG(Logger, "{:90}| Write -->", FUNCTION_LINE_NAME);

            WriteRequest request;
            request.Parameters.NodesToWrite = values;
            const WriteResponse response = Send<WriteResponse>(request);

            LOG_DEBUG(Logger, "{:90}| Write <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        ////////////////////////////////////////////////////////////////
        /// Endpoint Services
        ////////////////////////////////////////////////////////////////
        virtual std::shared_ptr<EndpointServices> Endpoints() override
        {
            return shared_from_this();
        }

        virtual std::vector<ApplicationDescription> FindServers(const FindServersParameters& params) const override
        {
            LOG_DEBUG(Logger, "{:90}| FindServers -->", FUNCTION_LINE_NAME);

            OpcUa::FindServersRequest request;
            request.Parameters = params;
            FindServersResponse response = Send<FindServersResponse>(request);

            LOG_DEBUG(Logger, "{:90}| FindServers <--", FUNCTION_LINE_NAME);

            return response.Data.Descriptions;
        }

        virtual std::vector<EndpointDescription> GetEndpoints(const GetEndpointsParameters& filter) const override
        {
            LOG_DEBUG(Logger, "{:90}| GetEndpoints -->", FUNCTION_LINE_NAME);

            OpcUa::GetEndpointsRequest request;
            request.Header = CreateRequestHeader();
            request.Parameters.EndpointUrl = filter.EndpointUrl;
            request.Parameters.LocaleIds = filter.LocaleIds;
            request.Parameters.ProfileUris = filter.ProfileUris;
            const GetEndpointsResponse response = Send<GetEndpointsResponse>(request);

            LOG_DEBUG(Logger, "{:90}| GetEndpoints <--", FUNCTION_LINE_NAME);

            return response.Endpoints;
        }

        virtual void RegisterServer(const ServerParameters& parameters) override
        {
        }

        ////////////////////////////////////////////////////////////////
        /// Method Services
        ////////////////////////////////////////////////////////////////
        virtual std::shared_ptr<MethodServices> Method() override
        {
            return shared_from_this();
        }

        virtual std::vector<CallMethodResult> Call(const std::vector<CallMethodRequest>& methodsToCall) override
        {
            LOG_DEBUG(Logger, "{:90}| Call -->", FUNCTION_LINE_NAME);

            CallRequest request;
            request.Parameters.MethodsToCall = methodsToCall;
            const CallResponse response = Send<CallResponse>(request);

            LOG_DEBUG(Logger, "{:90}| Call <--", FUNCTION_LINE_NAME);

            // Manage errors
        //       if (!response.DiagnosticInfos.empty())
        //       {
        // For now commented out, handling of diagnostic should be probably added for all communication
        //       }
            return response.Results;
        }

        ////////////////////////////////////////////////////////////////
        /// Node management Services
        ////////////////////////////////////////////////////////////////

        virtual std::shared_ptr<NodeManagementServices> NodeManagement() override
        {
            return shared_from_this();
        }

        virtual std::vector<AddNodesResult> AddNodes(const std::vector<AddNodesItem>& items) override
        {
            LOG_DEBUG(Logger, "{:90}| AddNodes -->", FUNCTION_LINE_NAME);

            AddNodesRequest request;
            request.Parameters.NodesToAdd = items;
            const AddNodesResponse response = Send<AddNodesResponse>(request);

            LOG_DEBUG(Logger, "{:90}| AddNodes <--", FUNCTION_LINE_NAME);

            return response.results;
        }

        virtual std::vector<StatusCode> AddReferences(const std::vector<AddReferencesItem>& items) override
        {
            LOG_DEBUG(Logger, "{:90}| AddReferences -->", FUNCTION_LINE_NAME);

            AddReferencesRequest request;
            request.Parameters.ReferencesToAdd = items;
            const AddReferencesResponse response = Send<AddReferencesResponse>(request);

            LOG_DEBUG(Logger, "{:90}| AddReferences <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        virtual void SetMethod(const NodeId& node, std::function<std::vector<OpcUa::Variant> (NodeId context, std::vector<OpcUa::Variant> arguments)> callback) override
        {
            LOG_WARN(Logger, "{:90}| SetMethod has no effect on client!", FUNCTION_LINE_NAME);

            return;
        }

        ////////////////////////////////////////////////////////////////
        /// Subscriptions Services
        ////////////////////////////////////////////////////////////////
        virtual std::shared_ptr<SubscriptionServices> Subscriptions() override
        {
            return shared_from_this();
        }

        virtual SubscriptionData CreateSubscription(const CreateSubscriptionRequest& request, std::function<void (PublishResult)> callback) override
        {
            LOG_DEBUG(Logger, "{:90}| CreateSubscription -->", FUNCTION_LINE_NAME);

            const CreateSubscriptionResponse response = Send<CreateSubscriptionResponse>(request);

            LOG_DEBUG(Logger, "{:90}| got CreateSubscriptionResponse", FUNCTION_LINE_NAME);

            PublishCallbacks[response.Data.SubscriptionId] = callback;// TODO Pass callback to the Publish method.

            LOG_DEBUG(Logger, "{:90}| CreateSubscription <--", FUNCTION_LINE_NAME);

            return response.Data;
        }

        virtual ModifySubscriptionResponse ModifySubscription(const ModifySubscriptionParameters& parameters) override
        {
            LOG_DEBUG(Logger, "{:90}| ModifySubscription -->", FUNCTION_LINE_NAME);

            ModifySubscriptionRequest request;
            request.Parameters = parameters;
            const ModifySubscriptionResponse response = Send<ModifySubscriptionResponse>(request);

            LOG_DEBUG(Logger, "{:90}| ModifySubscription <--", FUNCTION_LINE_NAME);

            return response;
        }

        virtual std::vector<StatusCode> DeleteSubscriptions(const std::vector<uint32_t>& subscriptions) override
        {
            LOG_DEBUG(Logger, "{:90}| DeleteSubscriptions -->", FUNCTION_LINE_NAME);

            DeleteSubscriptionsRequest request;
            request.SubscriptionIds = subscriptions;
            const DeleteSubscriptionsResponse response = Send<DeleteSubscriptionsResponse>(request);

            LOG_DEBUG(Logger, "{:90}| DeleteSubscriptions <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        virtual std::vector<MonitoredItemCreateResult> CreateMonitoredItems(const MonitoredItemsParameters& parameters) override
        {
            LOG_DEBUG(Logger, "{:90}| CreateMonitoredItems -->", FUNCTION_LINE_NAME);
            LOG_TRACE(Logger, "{:90}| {}", FUNCTION_LINE_NAME, parameters);

            CreateMonitoredItemsRequest request;
            request.Parameters = parameters;
            const CreateMonitoredItemsResponse response = Send<CreateMonitoredItemsResponse>(request);

            LOG_DEBUG(Logger, "{:90}| CreateMonitoredItems <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        virtual std::vector<StatusCode> DeleteMonitoredItems(const DeleteMonitoredItemsParameters& params) override
        {
            LOG_DEBUG(Logger, "{:90}| DeleteMonitoredItems -->", FUNCTION_LINE_NAME);

            DeleteMonitoredItemsRequest request;
            request.Parameters = params;
            const DeleteMonitoredItemsResponse response = Send<DeleteMonitoredItemsResponse>(request);

            LOG_DEBUG(Logger, "{:90}| DeleteMonitoredItems <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        virtual void Publish(const PublishRequest& originalrequest) override
        {
            LOG_DEBUG(Logger, "{:90}| Publish --> request with {} acks", FUNCTION_LINE_NAME, originalrequest.SubscriptionAcknowledgements.size());

            PublishRequest request(originalrequest);
            request.Header = CreateRequestHeader();
            request.Header.Timeout = 0; //We do not want the request to timeout!

            ResponseCallback responseCallback = [this](std::vector<char> buffer, ResponseHeader h)
            {
                LOG_DEBUG(Logger, "{:90}| got publish response, from server", FUNCTION_LINE_NAME);

                PublishResponse response;

                if(h.ServiceResult != OpcUa::StatusCode::Good)
                {
                    response.Header = std::move(h);
                }

                else
                {
                    BufferInputChannel bufferInput(buffer);
                    IStreamBinary in(bufferInput);
                    in >> response;
                }

                CallbackService.post([this, response]()
                {
                    if(response.Header.ServiceResult == OpcUa::StatusCode::Good)
                    {
                        LOG_DEBUG(Logger, "{:90}| calling callback for Subscription: {}", FUNCTION_LINE_NAME, response.Parameters.SubscriptionId);

                        SubscriptionCallbackMap::const_iterator callbackIt = this->PublishCallbacks.find(response.Parameters.SubscriptionId);

                        if(callbackIt == this->PublishCallbacks.end())
                        {
                            LOG_WARN(Logger, "{:90}| unknown SubscriptionId {}", FUNCTION_LINE_NAME, response.Parameters.SubscriptionId);
                        }

                        else
                        {
                            try   //calling client code, better put it under try/catch otherwise we crash entire client
                            {
                                callbackIt->second(response.Parameters);
                            }

                            catch(const std::exception& ex)
                            {
                                LOG_WARN(Logger, "{:90}| error calling application callback: {}", FUNCTION_LINE_NAME, ex.what());
                            }
                        }
                    }

                    else if(response.Header.ServiceResult == OpcUa::StatusCode::BadSessionClosed)
                    {
                        LOG_WARN(Logger, "{:90}| session is closed", FUNCTION_LINE_NAME);
                    }

                    else
                    {
                      // TODO
                        LOG_DEBUG(Logger, "{:90}| not implemented", FUNCTION_LINE_NAME);
                    }
                });
            };
            std::unique_lock<std::mutex> lock(Mutex);
            Callbacks.insert(std::make_pair(request.Header.RequestHandle, responseCallback));
            lock.unlock();
            Send(request);

            LOG_DEBUG(Logger, "{:90}| Publish  <--", FUNCTION_LINE_NAME);
        }

        virtual RepublishResponse Republish(const RepublishParameters& params) override
        {
            LOG_DEBUG(Logger, "{:90}| Republish -->", FUNCTION_LINE_NAME);

            RepublishRequest request;
            request.Header = CreateRequestHeader();
            request.Parameters = params;

            RepublishResponse response = Send<RepublishResponse>(request);

            LOG_DEBUG(Logger, "{:90}| Republish  <--", FUNCTION_LINE_NAME);

            return response;
        }

        ////////////////////////////////////////////////////////////////
        /// View Services
        ////////////////////////////////////////////////////////////////
        virtual std::shared_ptr<ViewServices> Views() override
        {
            return shared_from_this();
        }

        virtual std::vector<BrowsePathResult> TranslateBrowsePathsToNodeIds(const TranslateBrowsePathsParameters& params) const override
        {
            LOG_DEBUG(Logger, "{:90}| TranslateBrowsePathsToNodeIds -->", FUNCTION_LINE_NAME);

            TranslateBrowsePathsToNodeIdsRequest request;
            request.Header = CreateRequestHeader();
            request.Parameters = params;
            const TranslateBrowsePathsToNodeIdsResponse response = Send<TranslateBrowsePathsToNodeIdsResponse>(request);

            LOG_DEBUG(Logger, "{:90}| TranslateBrowsePathsToNodeIds <--", FUNCTION_LINE_NAME);

            return response.Result.Paths;
        }


        virtual std::vector<BrowseResult> Browse(const OpcUa::NodesQuery& query) const override
        {
            LOG_DEBUG(Logger, "{:90}| Browse -->", FUNCTION_LINE_NAME);
            if(Logger && Logger->should_log(spdlog::level::trace))
            {
                for(BrowseDescription desc : query.NodesToBrowse)
                {
                    Logger->trace("Node: {}", desc.NodeToBrowse);
                }
            }

            BrowseRequest request;
            request.Header = CreateRequestHeader();
            request.Query = query;
            const BrowseResponse response = Send<BrowseResponse>(request);
            ContinuationPoints.clear();

            for(BrowseResult result : response.Results)
            {
                if(!result.ContinuationPoint.empty())
                {
                    ContinuationPoints.push_back(result.ContinuationPoint);
                }
            }

            LOG_DEBUG(Logger, "{:90}| Browse <--", FUNCTION_LINE_NAME);

            return  response.Results;
        }

        virtual std::vector<BrowseResult> BrowseNext() const override
        {
            LOG_DEBUG(Logger, "{:90}| BrowseNext -->", FUNCTION_LINE_NAME);

            //FIXME: fix method interface so we do not need to decice arbitriraly if we need to send BrowseNext or not...
            if(ContinuationPoints.empty())
            {
                LOG_DEBUG(Logger, "{:90}| BrowseNext <-- no Continuation point, no need to send browse next request", FUNCTION_LINE_NAME);

                return std::vector<BrowseResult>();
            }

            BrowseNextRequest request;
            request.ReleaseContinuationPoints = ContinuationPoints.empty() ? true : false;
            request.ContinuationPoints = ContinuationPoints;
            const BrowseNextResponse response = Send<BrowseNextResponse>(request);
            ContinuationPoints.clear();

            for(auto result : response.Results)
            {
                if(!result.ContinuationPoint.empty())
                {
                    ContinuationPoints.push_back(result.ContinuationPoint);
                }
            }

            LOG_DEBUG(Logger, "{:90}| BrowseNext <--", FUNCTION_LINE_NAME);

            return response.Results;
        }

        std::vector<NodeId> RegisterNodes(const std::vector<NodeId>& params) const override
        {
            LOG_DEBUG(Logger, "{:90}| RegisterNodes -->", FUNCTION_LINE_NAME);
            if(Logger && Logger->should_log(spdlog::level::trace))
            {
                Logger->trace("{:90}| Nodes to register:", FUNCTION_LINE_NAME);

                for(auto& param : params)
                {
                    Logger->trace("    {}", param);
                }
            }

            RegisterNodesRequest request;

            request.NodesToRegister = params;
            RegisterNodesResponse response = Send<RegisterNodesResponse>(request);

            if(Logger && Logger->should_log(spdlog::level::trace))
            {
                Logger->trace("{:90}| registered NodeIds:", FUNCTION_LINE_NAME);

                for(auto& id : response.Result)
                {
                    Logger->trace("    {}", id);
                }
            }
            LOG_DEBUG(Logger, "{:90}| RegisterNodes <--", FUNCTION_LINE_NAME);
            return response.Result;
        }

        void UnregisterNodes(const std::vector<NodeId>& params) const override
        {
            LOG_DEBUG(Logger, "{:90}| UnregisterNodes -->", FUNCTION_LINE_NAME);
            if(Logger && Logger->should_log(spdlog::level::trace))
            {
                Logger->trace("{:90}| Nodes to unregister:", FUNCTION_LINE_NAME);

                for(auto& id : params)
                {
                    Logger->trace("    {}", id);
                }
            }

            UnregisterNodesRequest request;
            request.NodesToUnregister = params;
            UnregisterNodesResponse response = Send<UnregisterNodesResponse>(request);

            LOG_DEBUG(Logger, "{:90}| UnregisterNodes <--", FUNCTION_LINE_NAME);
        }

      //private:
        //FIXME: this method should be removed, better add realease option to BrowseNext
        void Release() const
        {
            ContinuationPoints.clear();
            BrowseNext();
        }

    public:

      ////////////////////////////////////////////////////////////////
      /// SecureChannel Services
      ////////////////////////////////////////////////////////////////
        virtual OpcUa::OpenSecureChannelResponse OpenSecureChannel(const OpenSecureChannelParameters& params) override
        {
            LOG_DEBUG(Logger, "{:90}| OpenChannel -->", FUNCTION_LINE_NAME);

            OpenSecureChannelRequest request;
            request.Parameters = params;

            OpenSecureChannelResponse response = Send<OpenSecureChannelResponse>(request);

            ChannelSecurityToken = response.ChannelSecurityToken; //Save security token, we need it

            LOG_DEBUG(Logger, "{:90}| OpenChannel <--", FUNCTION_LINE_NAME);

            return response;
        }

        virtual void CloseSecureChannel(uint32_t channelId) override
        {
            LOG_DEBUG(Logger, "{:90}| CloseSecureChannel -->", FUNCTION_LINE_NAME);
            try
            {
                SecureHeader hdr(MT_SECURE_CLOSE, CHT_SINGLE, ChannelSecurityToken.SecureChannelId);

                const SymmetricAlgorithmHeader algorithmHeader = CreateAlgorithmHeader();
                hdr.AddSize(RawSize(algorithmHeader));

                std::unique_lock<std::mutex> send_lock(send_mutex);

                const SequenceHeader sequence = CreateSequenceHeader();
                hdr.AddSize(RawSize(sequence));

                CloseSecureChannelRequest request;
                //request. ChannelId = channelId; FIXME: spec says it hsould be here, in practice it is not even sent?!?!
                hdr.AddSize(RawSize(request));

                Stream << hdr << algorithmHeader << sequence << request << flush;
            }

            catch(const std::exception& exc)
            {
                LOG_WARN(Logger, "closing secure channel failed with: {}", FUNCTION_LINE_NAME, exc.what());
            }

            LOG_DEBUG(Logger, "{:90}| CloseSecureChannel <--", FUNCTION_LINE_NAME);
        }

      //private:
        template <typename Response, typename Request>
        Response Send(Request request) const
        {
            request.Header = CreateRequestHeader();
            RequestCallback<Response> requestCallback(Logger);
            ResponseCallback responseCallback = [&requestCallback](std::vector<char> buffer, ResponseHeader h)
            {
                requestCallback.OnData(std::move(buffer), std::move(h));
            };
            std::unique_lock<std::mutex> lock(Mutex);
            Callbacks.insert(std::make_pair(request.Header.RequestHandle, responseCallback));
            lock.unlock();

            LOG_DEBUG(Logger, "{:90}| send: id: {} handle: {}, UtcTime: {}", FUNCTION_LINE_NAME, ToString(request.TypeId, true), request.Header.RequestHandle, request.Header.UtcTime);

            
            Send(request);

            Response res;

            try
            {
                LOG_CRITICAL(Logger, "{:90}| -->WaitForData {}", FUNCTION_LINE_NAME, request.Header.Timeout);
                res = requestCallback.WaitForData(std::chrono::milliseconds(request.Header.Timeout));
                LOG_CRITICAL(Logger, "{:90}| <--WaitForData {}", FUNCTION_LINE_NAME, request.Header.Timeout);
            }
            catch(std::exception& ex)
            {
                //Remove the callback on timeout
                std::unique_lock<std::mutex> lock(Mutex);
                Callbacks.erase(request.Header.RequestHandle);
                lock.unlock();
                throw;
            }

            return res;
        }


      // Prevent multiple threads from sending parts of different packets at the same time.
        mutable std::mutex send_mutex;

        template <typename Request>
        void Send(Request request) const
        {
          // TODO add support for breaking message into multiple chunks
            SecureHeader hdr(MT_SECURE_MESSAGE, CHT_SINGLE, ChannelSecurityToken.SecureChannelId);
            const SymmetricAlgorithmHeader algorithmHeader = CreateAlgorithmHeader();
            hdr.AddSize(RawSize(algorithmHeader));

            std::unique_lock<std::mutex> send_lock(send_mutex);

            const SequenceHeader sequence = CreateSequenceHeader();
            hdr.AddSize(RawSize(sequence));
            hdr.AddSize(RawSize(request));

            LOG_CRITICAL(Logger, "{:90}| -->Stream: {}", FUNCTION_LINE_NAME, hdr.Size);
            Stream << hdr << algorithmHeader << sequence << request << flush;
        }

        void Receive()
        {
            LOG_CRITICAL(Logger, "{:90}| -->Receive()", FUNCTION_LINE_NAME);
            
            Binary::SecureHeader responseHeader;
            LOG_CRITICAL(Logger, "{:90}| -->Stream: {}", FUNCTION_LINE_NAME, RawSize(responseHeader));
            Stream >> responseHeader;
            LOG_CRITICAL(Logger, "{:90}| <--Stream: received message: Type: {}, ChunkType: {}, Size: {}, ChannelId: {}", FUNCTION_LINE_NAME, responseHeader.Type, responseHeader.Chunk, responseHeader.Size, responseHeader.ChannelId);

            LOG_DEBUG(Logger, "{:90}| received message: Type: {}, ChunkType: {}, Size: {}, ChannelId: {}", FUNCTION_LINE_NAME, responseHeader.Type, responseHeader.Chunk, responseHeader.Size, responseHeader.ChannelId);

            size_t algo_size;

            if(responseHeader.Type == MessageType::MT_SECURE_OPEN)
            {
                AsymmetricAlgorithmHeader responseAlgo;
                LOG_CRITICAL(Logger, "{:90}| -->Stream: MT_SECURE_OPEN", FUNCTION_LINE_NAME, RawSize(responseAlgo));
                Stream >> responseAlgo;
                LOG_CRITICAL(Logger, "{:90}| <--Stream: MT_SECURE_OPEN", FUNCTION_LINE_NAME, RawSize(responseAlgo));
                algo_size = RawSize(responseAlgo);
            }

            else if(responseHeader.Type == MessageType::MT_ERROR)
            {
                StatusCode error;
                std::string msg;
                Stream >> error;
                Stream >> msg;
                std::stringstream stream;
                stream << "| Received error message from server: " << ToString(error) << ", " << msg;

                LOG_CRITICAL(Logger, "{:90}| -->Stream: MT_ERROR, Received error message from server: {}, {}", FUNCTION_LINE_NAME, ToString(error), msg);

                throw std::runtime_error(FUNCTION_LINE_NAME + stream.str());
            }

            else //(responseHeader.Type == MessageType::MT_SECURE_MESSAGE )
            {
                Binary::SymmetricAlgorithmHeader responseAlgo;
                LOG_CRITICAL(Logger, "{:90}| -->Stream: responseAlgo {}", FUNCTION_LINE_NAME, RawSize(responseAlgo));
                Stream >> responseAlgo;
                algo_size = RawSize(responseAlgo);
                LOG_CRITICAL(Logger, "{:90}| <--Stream: responseAlgo {}", FUNCTION_LINE_NAME, algo_size);
            }

            NodeId id;
            Binary::SequenceHeader responseSequence;
            LOG_CRITICAL(Logger, "{:90}| -->Stream: SequenceHeader {}", FUNCTION_LINE_NAME, RawSize(responseSequence));
            Stream >> responseSequence; // TODO Check for request Number
            LOG_CRITICAL(Logger, "{:90}| <--Stream: SequenceHeader {}", FUNCTION_LINE_NAME, RawSize(responseSequence));

            const std::size_t expectedHeaderSize = RawSize(responseHeader) + algo_size + RawSize(responseSequence);

            if(expectedHeaderSize >= responseHeader.Size)
            {
                std::stringstream stream;
                LOG_CRITICAL(Logger, "{:90}| <--Stream: Error SequenceHeader {} >= {}", FUNCTION_LINE_NAME, expectedHeaderSize,  responseHeader.Size);
                stream << "| Size of received message " << responseHeader.Size << " bytes is invalid. Expected size " << expectedHeaderSize << " bytes";
                throw std::runtime_error(FUNCTION_LINE_NAME + stream.str());
            }

            std::size_t dataSize = responseHeader.Size - expectedHeaderSize;

            if(responseHeader.Chunk == CHT_SINGLE)
            {
                parseMessage(dataSize, id);
                firstMsgParsed = false;

                std::unique_lock<std::mutex> lock(Mutex);
                CallbackMap::const_iterator callbackIt = Callbacks.find(header.RequestHandle);

                if(callbackIt == Callbacks.end())
                {
                    LOG_WARN(Logger, "{:90}| no callback found for message id: {}, handle: {}", FUNCTION_LINE_NAME, id, header.RequestHandle);
                    messageBuffer.clear();
                    return;
                }

                callbackIt->second(std::move(messageBuffer), std::move(header));
                messageBuffer.clear();
                Callbacks.erase(callbackIt);
            }

            else if(responseHeader.Chunk == CHT_INTERMEDIATE)
            {
                parseMessage(dataSize, id);
                firstMsgParsed = true;
            }
            LOG_CRITICAL(Logger, "{:90}| <--Receive()", FUNCTION_LINE_NAME);
        }

        void parseMessage(std::size_t& dataSize, NodeId& id)
        {
            std::vector<char> buffer(dataSize);
            BufferInputChannel bufferInput(buffer);
            Binary::RawBuffer raw(&buffer[0], dataSize);
            Stream >> raw;
            LOG_TRACE(Logger, "{:90}| received message data: {}", FUNCTION_LINE_NAME, ToHexDump(buffer));

            if(!firstMsgParsed)
            {
                IStreamBinary in(bufferInput);
                in >> id;
                in >> header;

                LOG_DEBUG(Logger, "{:90}| got response id: {}, handle: {}", FUNCTION_LINE_NAME, ToString(id, true), header.RequestHandle);

                if(id == SERVICE_FAULT)
                {
                    LOG_WARN(Logger, "{:90}| receive ServiceFault from Server with StatusCode: {}", FUNCTION_LINE_NAME, header.ServiceResult);
                }
                else if(header.ServiceResult != StatusCode::Good)
                {
                    LOG_WARN(Logger, "{:90}| received a response from server with error status: {}", FUNCTION_LINE_NAME, header.ServiceResult);
                }

                messageBuffer.insert(messageBuffer.end(), buffer.begin(), buffer.end());
            }

            else
            {
                messageBuffer.insert(messageBuffer.end(), buffer.begin(), buffer.end());
            }
        }

        Binary::Acknowledge HelloServer(const SecureConnectionParams& params)
        {
            LOG_DEBUG(Logger, "{:90}| HelloServer -->", FUNCTION_LINE_NAME);

            Binary::Hello hello;
            hello.ProtocolVersion = 0;
            hello.ReceiveBufferSize = 65536;
            hello.SendBufferSize = 65536;
            hello.MaxMessageSize = 65536;
            hello.MaxChunkCount = 256;
            hello.EndpointUrl = params.EndpointUrl;

            Binary::Header hdr(Binary::MT_HELLO, Binary::CHT_SINGLE);
            hdr.AddSize(RawSize(hello));

            Stream << hdr << hello << flush;

            Header respHeader;
            Stream >> respHeader; // TODO add check for acknowledge header

            Acknowledge ack;
            Stream >> ack; // TODO check for connection parameters

            LOG_DEBUG(Logger, "{:90}| HelloServer <--", FUNCTION_LINE_NAME);

            return ack;
        }


        SymmetricAlgorithmHeader CreateAlgorithmHeader() const
        {
            SymmetricAlgorithmHeader algorithmHeader;
            algorithmHeader.TokenId = ChannelSecurityToken.TokenId;
            return algorithmHeader;
        }

        SequenceHeader CreateSequenceHeader() const
        {
            SequenceHeader sequence;
            sequence.SequenceNumber = ++SequenceNumber;
            sequence.RequestId = ++RequestNumber;
            return sequence;
        }

        RequestHeader CreateRequestHeader() const
        {
            RequestHeader header;
            header.SessionAuthenticationToken = AuthenticationToken;
            header.RequestHandle = GetRequestHandle();
            header.Timeout = 10000;
            return header;
        }

        unsigned GetRequestHandle() const
        {
            return ++RequestHandle;
        }

        // Binary client is self-referenced from captures of subscription callbacks
        // Remove this references to make ~BinaryClient() run possible
        void RemoveSelfReferences()
        {
            LOG_DEBUG(Logger, "{:90}| clearing cached references to server", FUNCTION_LINE_NAME);

            PublishCallbacks.clear();
        }

    //private:
        std::shared_ptr<IOChannel> Channel;
        mutable IOStreamBinary Stream;
        SecureConnectionParams Params;
        std::thread ReceiveThread;

        SubscriptionCallbackMap PublishCallbacks;
        SecurityToken ChannelSecurityToken;
        mutable std::atomic<uint32_t> SequenceNumber;
        mutable std::atomic<uint32_t> RequestNumber;
        ExpandedNodeId AuthenticationToken;
        mutable std::atomic<uint32_t> RequestHandle;
        mutable std::vector<std::vector<uint8_t>> ContinuationPoints;
        mutable CallbackMap Callbacks;
        Common::Logger::SharedPtr Logger;
        bool Finished = false;

        std::thread callback_thread;
        CallbackThread CallbackService;
        mutable std::mutex Mutex;

        bool firstMsgParsed = false;
        ResponseHeader header;
    };

    template <>
    void BinaryClient::Send<OpenSecureChannelRequest>(OpenSecureChannelRequest request) const
    {
        SecureHeader hdr(MT_SECURE_OPEN, CHT_SINGLE, ChannelSecurityToken.SecureChannelId);
        AsymmetricAlgorithmHeader algorithmHeader;
        algorithmHeader.SecurityPolicyUri = Params.SecurePolicy;
        algorithmHeader.SenderCertificate = Params.SenderCertificate;
        algorithmHeader.ReceiverCertificateThumbPrint = Params.ReceiverCertificateThumbPrint;
        hdr.AddSize(RawSize(algorithmHeader));
        hdr.AddSize(RawSize(request));

        std::unique_lock<std::mutex> send_lock(send_mutex);

        const SequenceHeader sequence = CreateSequenceHeader();
        hdr.AddSize(RawSize(sequence));

        LOG_CRITICAL(Logger, "{:90}| -->Stream: {}", FUNCTION_LINE_NAME, hdr.Size);
        Stream << hdr << algorithmHeader << sequence << request << flush;
        LOG_CRITICAL(Logger, "{:90}| <--Stream: {}", FUNCTION_LINE_NAME, hdr.Size);
    }

}// namespace


OpcUa::Services::SharedPtr OpcUa::CreateBinaryClient(OpcUa::IOChannel::SharedPtr channel, const OpcUa::SecureConnectionParams & params, const Common::Logger::SharedPtr & logger)
{
  return std::make_shared<BinaryClient>(channel, params, logger);
}

OpcUa::Services::SharedPtr OpcUa::CreateBinaryClient(const std::string & endpointUrl, const Common::Logger::SharedPtr & logger)
{
  const Common::Uri serverUri(endpointUrl);
  OpcUa::IOChannel::SharedPtr channel = OpcUa::Connect(serverUri.Host(), serverUri.Port(), logger);
  OpcUa::SecureConnectionParams params;
  params.EndpointUrl = endpointUrl;
  params.SecurePolicy = "http://opcfoundation.org/UA/SecurityPolicy#None";
  return CreateBinaryClient(channel, params, logger);
}
