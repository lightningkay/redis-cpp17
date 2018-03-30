#include "xHiredisAsync.h"

std::atomic<int32_t> sessionCount = 0;
std::atomic<int32_t> gconnetCount = 0;
std::atomic<int32_t> sconnetCount = 0;
int32_t  benchCount  = 10000;

xHiredisAsync::xHiredisAsync(xEventLoop *loop,int threadCount,const char *ip,int16_t port)
:hiredis(loop),
connectCount(0),
loop(loop),
cron(true)
{
	hiredis.setThreadNum(threadCount);
	hiredis.start();

	for(int i = 0; i < sessionCount; i++)
	{
		TcpClientPtr client(new xTcpClient(hiredis.getPool().getNextLoop(),ip,port,nullptr));
		client->setConnectionCallback(std::bind(&xHiredisAsync::redisConnCallBack,this,std::placeholders::_1));
		client->setMessageCallback(std::bind(&xHiredis::redisReadCallBack,&hiredis,std::placeholders::_1,std::placeholders::_2));
		client->asyncConnect();
		hiredis.pushTcpClient(client);
	}

	std::unique_lock<std::mutex> lk(hiredis.getMutex());
	while (connectCount < sessionCount)
	{
		condition.wait(lk);
	}

	sessionCount = sessionCount * benchCount;
}

xHiredisAsync::~xHiredisAsync()
{

}

void xHiredisAsync::redisConnCallBack(const TcpConnectionPtr &conn)
{
	if(conn->connected())
	{
		RedisAsyncContextPtr ac(new xRedisAsyncContext(conn->intputBuffer(),conn));
		hiredis.insertRedisMap(conn->getSockfd(),ac);
		connectCount++;
		condition.notify_one();
	}
	else
	{
		if(--connectCount == 0)
		{
			test_cond(true);
			hiredis.clearTcpClient();
		}
		hiredis.eraseRedisMap(conn->getSockfd());
	}
}

void xHiredisAsync::setCallback(const RedisAsyncContextPtr &c,redisReply *reply,const std::any &privdata)
{
	assert(reply != nullptr);
	if(reply->type == REDIS_REPLY_ERROR || reply->type == REDIS_REPLY_NIL)
	{
		assert(false);
	}

	std::thread::id threadId = std::any_cast< std::thread::id>(privdata);
	assert(threadId == std::this_thread::get_id());
	sconnetCount++;
}

void xHiredisAsync::getCallback(const RedisAsyncContextPtr &c,redisReply *reply,const std::any &privdata)
{
	assert(reply != nullptr);
	if(reply->type == REDIS_REPLY_ERROR || reply->type == REDIS_REPLY_NIL)
	{
		assert(false);
	}

	std::thread::id threadId = std::any_cast< std::thread::id>(privdata);
	assert(threadId == std::this_thread::get_id());
	if(++gconnetCount == sessionCount  && sconnetCount == sessionCount)
	{
		test_cond(true);
	}
}

void xHiredisAsync::serverCron(const std::any &context)
{
	if(cron && gconnetCount == sessionCount  && sconnetCount == sessionCount)
	{
		test("Redis async close safe test");
		{
			std::unique_lock<std::mutex> lk(hiredis.getMutex());
			for(auto &it : hiredis.getTcpClient())
			{
				it->disConnect();
			}
		}
		cron = false;
	}
}

xAsyncLogging *g_asyncLog;
void asyncOutput(const char *msg, int len)
{
	printf("%s\n",msg);
	g_asyncLog->append(msg, len);
}

 int main(int argc, char* argv[])
 {
 	if (argc != 5)
 	{
 		fprintf(stderr, "Usage: client <host_ip> <port> <sessionCount> <threadCount> \n ");
 	}
 	else
 	{
		xLogger::setOutput(asyncOutput);
		xAsyncLogging log("hiredis", 4096);
		log.start();
		g_asyncLog = &log;

 		const char* ip = argv[1];
 		uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
 		sessionCount = atoi(argv[3]);
 		int threadCount = atoi(argv[4]);
 		xEventLoop loop;
		xHiredisAsync async(&loop,threadCount,ip,port);

		test("Redis async Connect test ");
		test_cond(true);
		test("Redis async multithreaded safe test ");

		{
			std::unique_lock<std::mutex> lk(async.getHiredis()->getMutex());
			auto  &redisMap = async.getHiredis()->getRedis();
			int32_t count = 0;
			for(auto it = redisMap.begin(); it != redisMap.end();)
			{
				if(count++ >= sessionCount)
				{
					break;
				}

				auto redis = it->second;
				std::thread::id  threadId = redis->getServerConn()->getLoop()->getThreadId();

				redis->redisAsyncCommand(std::bind(&xHiredisAsync::setCallback,
						&async,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3),
						threadId,"set key%d %d",count,count);

				redis->redisAsyncCommand(std::bind(&xHiredisAsync::getCallback,
						&async,std::placeholders::_1,std::placeholders::_2,std::placeholders::_3),
						threadId,"get key%d",count);

				if(++it == redisMap.end())
				{
					it = redisMap.begin();
				}
			}
		}

		loop.runAfter(1.0,nullptr,true,std::bind(&xHiredisAsync::serverCron,&async,std::placeholders::_1));
 		loop.run();
 	}
 	return 0;
 }


