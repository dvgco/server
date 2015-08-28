#include "DBSvr.h"

DBSvr::DBSvr()
{
    m_Config.init("./config/DBSvr.xml");
    m_Config.parseXml();

    m_nInterval = m_Config.m_accConfig.updateFps; //loop per Xms default
    m_nCycleTick = acct_time::getCurTimeMs(); 
    MAXPKGLEN = m_Config.m_accConfig.sessionbuflen;
    SESSIONBUFLEN = m_Config.m_accConfig.maxpkglen;

    m_nStatisticTick = m_nCycleTick;
    m_nHandleCount = 0;

    m_nNextTick = m_nCycleTick + m_nInterval;
    m_ServerID = m_Config.m_accConfig.serverid;
    m_nIoThreadNum = m_Config.m_accConfig.recvThread;
    LOGI("=====e======m_nIoThreadNum:" <<  m_nIoThreadNum);
    m_svrType = DBSvr;
    m_epollfd = epoll_create(10);
    m_epollSendfd = epoll_create(10);

    if (m_epollfd <= 0 || m_epollSendfd <= 0)
    {
        printf("DBSvr create epollfd error!!!");
        assert(false);
    }
}

DBSvr::~DBSvr()
{

}

void DBSvr::start()
{
    m_acceptor.init(m_Config.m_accConfig.maxclient);
    m_acceptor.startListen(m_Config.m_accConfig.ip, m_Config.m_accConfig.port);
    m_acceptor.start();

    for (int i = 0; i < m_nIoThreadNum;i++)
    {
        CIoThread *newThread = new CIoThread(this);
        newThread->start();
    }

    CSendThread *sendThread = new CSendThread(this);
    sendThread->start();
}

void DBSvr::updateSessionList()
{
    // 1.process accept session
    CommonList<CSession>* readList = m_acceptor.getReadSessionList();
    CommonList<CSession>::iterator it = readList->begin();
    for (; it != readList->end(); it++)
    {
        CSession *newSession = *it;
        newSession->setStatus(active);
        m_activeSessionList.push_back(newSession);
        //add to epoll event loop
        addFdToRecvEpoll(newSession);
        addFdToSendEpoll(newSession);
    }

    readList->clear();

    if (m_acceptor.getWriteSessionList()->size() > 0 )
    {
        m_acceptor.swapSessionList();
    }
    
    // 2.process connect session
    CommonList<CSession> *connSessionList =  m_connector.getConnList(); //all connector's session  connect to other server, we use mutilMap to record them
    if (connSessionList->size() > 0)
    {
        std::vector<CSession *> connectSessions;
        m_connector.getConnList(connectSessions);
        std::vector<CSession *>::iterator iter = connectSessions.begin();
        for (; iter != connectSessions.end(); iter++)
        {
            CSession *newSession = *iter;
            newSession->setStatus(active);
            m_activeSessionList.push_back(newSession);
            m_ServerSessionMap.insert(std::make_pair<SESSION_TYPE, CSession*>(newSession->getType(), newSession));
            //add to epoll event loop
            addFdToRecvEpoll(newSession);
            addFdToSendEpoll(newSession);
        }
    }

}

void DBSvr::removeDeadSession()
{
    if (m_activeSessionList.size() > 0)
    {
        CommonList<CSession>::iterator iter;
        for (iter = m_activeSessionList.begin(); iter != m_activeSessionList.end();)
        {
            CSession *session = *iter;
            if (session->getStatus() == waitdel)
            {
                session->delEpollEvent(m_epollfd);
                session->delEpollEvent(m_epollSendfd);
                session->clear();
                m_activeSessionList.erase(iter++);
                cout << "remove session===========" << session->getSessionId() << endl;
                if (session->getType() == eClient)
                {
                    m_acceptor.sessionReUse(session);
                }
                else
                {
                    typedef std::multimap<SESSION_TYPE, CSession *>::iterator mapiter;
                    typedef std::pair<mapiter, mapiter> rangeBeginEnd;
                    rangeBeginEnd range = m_ServerSessionMap.equal_range(session->getType());
                    for (mapiter be = range.first; be != range.second; be++)
                    {
                        if (be->second->getSessionId() == session->getSessionId())
                        {
                            m_ServerSessionMap.erase(be);
                            //put in connector errrolist, wait for reconnect...
                            m_connector.addToErrorList(session);
                            break;
                        }
                    }
                }
            }
            else
            {
                iter++;
            }
        }
    }
}

void DBSvr::handleActiveSession()
{
    if (m_activeSessionList.size() > 0)
    {
        CommonList<CSession>::iterator iter = m_activeSessionList.begin();
        for (; iter != m_activeSessionList.end(); iter++)
        {
            CSession *session = *iter;
            session->processPacket();
            if ((acct_time::getCurTimeMs() - m_nStatisticTick)>1000) //1s
            {
                m_nStatisticTick = acct_time::getCurTimeMs() + 1000;
                //cout << "========================================session=====" << ++m_nHandleCount << endl;
                m_nHandleCount = 0;
            }
            m_nHandleCount++;
        }
    }
}

void DBSvr::update()
{
    while (true)
    {
        while (acct_time::getCurTimeMs() >= m_nNextTick)
        {
            m_nNextTick = acct_time::getCurTimeMs() + m_nInterval;
            updateSessionList(); // handle new Session
            handleActiveSession();
            removeDeadSession();
            // 30 ms per logic handle
            //cout << "into logic loop:" << acct_time::getCurTimeMs() << endl;
        }
        //cout << "======out of frame!!!" << endl;
        acct_time::sleepMs(10); // sleep 1ms per loop
    }
}
