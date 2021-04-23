//
//  TaskManager.cpp
//  WechatExporter
//
//  Created by Matthew on 2021/4/20.
//  Copyright © 2021 Matthew. All rights reserved.
//

#include "TaskManager.h"
#include "AsyncTask.h"

TaskManager::TaskManager() : m_downloadExecutor(4, 8, this), m_copyExecutor(1, 1, this), m_mp3Executor(2, 4, this), m_pdfExecutor(4, 4, this)
{
#ifndef NDEBUG
    m_downloadExecutor.setTag("dl");
    m_copyExecutor.setTag("cp");
    m_mp3Executor.setTag("mp3");
    m_pdfExecutor.setTag("pdf");
#endif
}

TaskManager::~TaskManager()
{
}

void TaskManager::setUserAgent(const std::string& userAgent)
{
    m_userAgent = userAgent;
}

void TaskManager::onTaskStart(const AsyncExecutor* executor, const AsyncExecutor::Task *task)
{
}

void TaskManager::onTaskComplete(const AsyncExecutor* executor, const AsyncExecutor::Task *task, bool succeeded)
{
    const Session* session = task->getUserData() == NULL ? NULL : reinterpret_cast<const Session *>(task->getUserData());
    if (executor == &m_downloadExecutor)
    {
        const DownloadTask* downloadTask = dynamic_cast<const DownloadTask *>(task);
        
        std::unique_lock<std::mutex> lock(m_mutex);
        std::map<std::string, uint32_t>::const_iterator it = m_downloadingTasks.find(downloadTask->getUrl());
        if (it != m_downloadingTasks.cend())
        {
            m_downloadingTasks.erase(it);
        }
        
        std::set<AsyncExecutor::Task *> copyTasks = dequeueCopyTasks(task->getTaskId());
        
        // check copy task
        AsyncExecutor::Task *pdfTask = NULL;
        uint32_t taskCount = decreaseSessionTask(session);
        if (0 == taskCount)
        {
            pdfTask = dequeuePdfTasks(session);
        }
        
        lock.unlock();
        for (std::set<AsyncExecutor::Task *>::iterator it = copyTasks.begin(); it != copyTasks.end(); ++it)
        {
            m_copyExecutor.addTask(*it);
        }
        if (NULL != pdfTask)
        {
            m_pdfExecutor.addTask(pdfTask);
        }
    }
    else if (executor == &m_copyExecutor || executor == &m_mp3Executor)
    {
        // check copy task
        AsyncExecutor::Task *pdfTask = NULL;
        std::unique_lock<std::mutex> lock(m_mutex);
        uint32_t taskCount = decreaseSessionTask(session);
        if (0 == taskCount)
        {
            pdfTask = dequeuePdfTasks(session);
        }
        
        lock.unlock();
        if (NULL != pdfTask)
        {
            m_pdfExecutor.addTask(pdfTask);
        }
    }
    else if (executor == &m_pdfExecutor)
    {
        
    }
}

void TaskManager::download(const Session* session, const std::string &url, const std::string& output, time_t mtime, const std::string& defaultFile/* = ""*/, std::string type/* = ""*/)
{
    std::map<std::string, std::string>::iterator it = m_downloadTasks.find(url);
    if (it != m_downloadTasks.end() && it->second == output)
    {
        // Existed and same output path, skip it
        return;
    }
    
    bool downloadFile = false;
    uint32_t taskId = AsyncExecutor::genNextTaskId();
    AsyncExecutor::Task *task = NULL;
    if (it != m_downloadTasks.end())
    {
        // Existed and different output path, copy it
        task = new CopyTask(it->second, output);
    }
    else
    {
        DownloadTask* downloadTask = new DownloadTask(url, output, defaultFile, mtime);
        downloadTask->setUserAgent(m_userAgent);
        task = downloadTask;
        downloadFile = true;
        m_downloadTasks.insert(std::pair<std::string, std::string>(url, output));
    }
    task->setTaskId(taskId);
    task->setUserData(reinterpret_cast<const void *>(session));
    
    std::unique_lock<std::mutex> lock(m_mutex);
    if (NULL != session)
    {
        std::map<const Session*, uint32_t>::iterator it2 = m_sessionTaskCount.find(session);
        if (it2 == m_sessionTaskCount.end())
        {
            m_sessionTaskCount.insert(std::pair<const Session*, uint32_t>(session, 1u));
        }
        else
        {
            it2->second++;
        }
    }
    
    if (downloadFile)
    {
        m_downloadingTasks.insert(std::pair<std::string, uint32_t>(url, taskId));
    }
    else
    {
        std::map<std::string, uint32_t>::const_iterator it3 = m_downloadingTasks.find(url);
        if (it3 != m_downloadingTasks.cend())
        {
            // Downloading is running, add waiting queue
            std::map<uint32_t, std::set<AsyncExecutor::Task *>>::iterator it4 = m_copyTaskQueue.find(it3->second);
            if (it4 == m_copyTaskQueue.end())
            {
                it4 = m_copyTaskQueue.insert(it4, std::pair<uint32_t, std::set<AsyncExecutor::Task *>>(it3->second, std::set<AsyncExecutor::Task *>()));
            }
            it4->second.insert(task);
            task = NULL;
        }
    }

    lock.unlock();
    if (NULL != task)
    {
        if (downloadFile)
        {
            m_downloadExecutor.addTask(task);
        }
        else
        {
            m_copyExecutor.addTask(task);
        }
        
    }
}

void TaskManager::convertMp3(const Session* session, const std::string& pcmPath, const std::string& mp3Path, unsigned int mtime)
{
    if (NULL == session)
    {
        return;
    }
    
    Mp3Task *task = new Mp3Task(pcmPath, mp3Path, mtime);
    task->setTaskId(AsyncExecutor::genNextTaskId());
    task->setUserData(reinterpret_cast<const void *>(session));
    
    std::unique_lock<std::mutex> lock(m_mutex);
    std::map<const Session*, uint32_t>::iterator it = m_sessionTaskCount.find(session);
    if (it == m_sessionTaskCount.end())
    {
        m_sessionTaskCount.insert(std::pair<const Session*, uint32_t>(session, 1u));
    }
    else
    {
        it->second++;
    }
    
    lock.unlock();
    
    m_mp3Executor.addTask(task);
}

void TaskManager::convertPdf(const Session* session, const std::string& htmlPath, const std::string& pdfPath, PdfConverter* pdfConverter)
{
    if (NULL == session)
    {
        return;
    }
    
    PdfTask *task = new PdfTask(pdfConverter, htmlPath, pdfPath);
    task->setTaskId(AsyncExecutor::genNextTaskId());
    task->setUserData(reinterpret_cast<const void *>(session));
    
    std::unique_lock<std::mutex> lock(m_mutex);
    std::map<const Session*, uint32_t>::iterator it = m_sessionTaskCount.find(session);
    if (it != m_sessionTaskCount.end() && it->second != 0)
    {
        m_pdfTaskQueue.insert(std::pair<const Session*, AsyncExecutor::Task *>(session, task));
    }
    else
    {
        lock.unlock();
        m_pdfExecutor.addTask(task);
    }
    
    
}