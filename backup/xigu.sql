-- MySQL dump 10.13  Distrib 9.7.4, for Linux (aarch64)
--
-- Host: localhost    Database: xigu
-- ------------------------------------------------------
-- Server version	9.7.4

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!50503 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;

--
-- Table structure for table `chat_message`
--

DROP TABLE IF EXISTS `chat_message`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `chat_message` (
  `message_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `thread_id` bigint unsigned NOT NULL,
  `sender_id` int NOT NULL,
  `recv_id` int NOT NULL,
  `client_msg_id` varchar(64) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `content` text COLLATE utf8mb4_unicode_ci NOT NULL,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `state` tinyint unsigned NOT NULL DEFAULT '0',
  `upload_state` tinyint unsigned NOT NULL DEFAULT '1',
  `msg_type` tinyint unsigned NOT NULL DEFAULT '0',
  `mentions` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`message_id`),
  UNIQUE KEY `uk_chat_client_msg` (`thread_id`,`sender_id`,`client_msg_id`),
  KEY `idx_chat_message_thread` (`thread_id`,`message_id`),
  CONSTRAINT `fk_message_conversation` FOREIGN KEY (`thread_id`) REFERENCES `conversation` (`thread_id`) ON DELETE CASCADE
) ENGINE=InnoDB AUTO_INCREMENT=315 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `chat_message`
--

LOCK TABLES `chat_message` WRITE;
/*!40000 ALTER TABLE `chat_message` DISABLE KEYS */;
INSERT INTO `chat_message` VALUES (1,1,1003,1004,'friend-welcome-1003-1004-1794834058','你好，我是test1','2026-11-16 13:00:58',0,1,0,''),(2,1,1004,1003,'friend-welcome-1004-1003-1794834058','好友验证通过了，随时可以找我聊天','2026-11-16 13:00:58',0,1,0,''),(3,1,1004,1003,'{13896843-b65f-44bf-bed3-5fe2bd1c35ce}','你好','2026-11-16 13:01:26',0,1,0,''),(4,1,1004,1003,'{577d250f-6787-48c5-9094-85e23f46fd57}','1','2026-11-16 13:03:09',0,1,0,''),(5,1,1003,1004,'{1bf81bba-2f25-46b7-add5-dedabc34efac}','1','2026-11-16 13:04:18',0,1,0,''),(6,1,1004,1003,'{07ea059e-2b5c-4692-a1a7-69a2ff11a485}','123','2026-11-16 13:04:34',0,1,0,''),(7,1,1003,1004,'{ce676104-135b-4c49-bae9-104d9054ffa7}','123','2026-11-16 13:04:44',0,1,0,''),(9,1,1003,1004,'{92f6e99a-b5b5-40cd-9d3a-acc6ac3327a6}','1','2026-11-16 13:17:47',0,1,0,'');
/*!40000 ALTER TABLE `chat_message` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `chat_message_archive`
--

DROP TABLE IF EXISTS `chat_message_archive`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `chat_message_archive` (
  `message_id` bigint unsigned NOT NULL,
  `thread_id` bigint unsigned NOT NULL,
  `sender_id` int NOT NULL,
  `recv_id` int NOT NULL DEFAULT '0',
  `client_msg_id` varchar(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `content` text CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL,
  `msg_type` tinyint unsigned NOT NULL DEFAULT '0',
  `state` tinyint unsigned NOT NULL DEFAULT '0',
  `upload_state` tinyint unsigned NOT NULL DEFAULT '1',
  `mentions` varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `archived_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`message_id`),
  KEY `idx_archive_thread` (`thread_id`,`message_id`),
  KEY `idx_archive_time` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `chat_message_archive`
--

LOCK TABLES `chat_message_archive` WRITE;
/*!40000 ALTER TABLE `chat_message_archive` DISABLE KEYS */;
/*!40000 ALTER TABLE `chat_message_archive` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `conversation`
--

DROP TABLE IF EXISTS `conversation`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `conversation` (
  `thread_id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `thread_type` tinyint unsigned NOT NULL DEFAULT '1',
  `dm_key` varchar(32) COLLATE utf8mb4_unicode_ci DEFAULT NULL,
  `name` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `owner_id` int NOT NULL DEFAULT '0',
  `state` tinyint unsigned NOT NULL DEFAULT '0' COMMENT '0 æ­£å¸¸ 1 å·²è§£æ•£',
  `dissolved_at` datetime DEFAULT NULL,
  `notice` varchar(500) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `last_message_id` bigint unsigned NOT NULL DEFAULT '0',
  `last_message_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`),
  UNIQUE KEY `uk_conversation_dm` (`dm_key`),
  KEY `idx_conversation_last` (`last_message_at`,`thread_id`)
) ENGINE=InnoDB AUTO_INCREMENT=13 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `conversation`
--

LOCK TABLES `conversation` WRITE;
/*!40000 ALTER TABLE `conversation` DISABLE KEYS */;
INSERT INTO `conversation` VALUES (1,1,'1003:1004','',0,0,NULL,'',311,'2026-11-19 06:48:14','2026-11-16 13:00:58');
/*!40000 ALTER TABLE `conversation` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `conversation_member`
--

DROP TABLE IF EXISTS `conversation_member`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `conversation_member` (
  `thread_id` bigint unsigned NOT NULL,
  `uid` int NOT NULL,
  `role` tinyint unsigned NOT NULL DEFAULT '0',
  `nickname` varchar(32) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `muted` tinyint unsigned NOT NULL DEFAULT '0',
  `last_read_message_id` bigint unsigned NOT NULL DEFAULT '0',
  `joined_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`thread_id`,`uid`),
  KEY `idx_member_uid` (`uid`,`thread_id`),
  CONSTRAINT `fk_member_conversation` FOREIGN KEY (`thread_id`) REFERENCES `conversation` (`thread_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `conversation_member`
--

LOCK TABLES `conversation_member` WRITE;
/*!40000 ALTER TABLE `conversation_member` DISABLE KEYS */;
INSERT INTO `conversation_member` VALUES (1,1003,0,'',0,310,'2026-11-18 11:33:59'),(1,1004,0,'',0,6,'2026-11-18 11:33:59');
/*!40000 ALTER TABLE `conversation_member` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `friend`
--

DROP TABLE IF EXISTS `friend`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `friend` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `self_id` int NOT NULL,
  `friend_id` int NOT NULL,
  `back` varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci DEFAULT '',
  `tags` varchar(512) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '[]',
  PRIMARY KEY (`id`) USING BTREE,
  UNIQUE KEY `self_friend` (`self_id`,`friend_id`) USING BTREE
) ENGINE=InnoDB AUTO_INCREMENT=5 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `friend`
--

LOCK TABLES `friend` WRITE;
/*!40000 ALTER TABLE `friend` DISABLE KEYS */;
INSERT INTO `friend` VALUES (1,1003,1004,'','[\"朋友\"]'),(2,1004,1003,'','[]');
/*!40000 ALTER TABLE `friend` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `friend_apply`
--

DROP TABLE IF EXISTS `friend_apply`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `friend_apply` (
  `id` bigint NOT NULL AUTO_INCREMENT,
  `from_uid` int NOT NULL,
  `to_uid` int NOT NULL,
  `status` smallint NOT NULL DEFAULT '0',
  `descs` varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `back_name` varchar(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `tags` varchar(512) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '[]',
  PRIMARY KEY (`id`) USING BTREE,
  UNIQUE KEY `from_to_uid` (`from_uid`,`to_uid`) USING BTREE
) ENGINE=InnoDB AUTO_INCREMENT=3 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `friend_apply`
--

LOCK TABLES `friend_apply` WRITE;
/*!40000 ALTER TABLE `friend_apply` DISABLE KEYS */;
INSERT INTO `friend_apply` VALUES (1,1003,1004,1,'你好，我是test1','','[]');
/*!40000 ALTER TABLE `friend_apply` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Table structure for table `user`
--

DROP TABLE IF EXISTS `user`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!50503 SET character_set_client = utf8mb4 */;
CREATE TABLE `user` (
  `uid` int NOT NULL AUTO_INCREMENT,
  `name` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `email` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `pwd` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `nick` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `desc` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  `sex` int NOT NULL DEFAULT '0',
  `icon` varchar(255) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '',
  PRIMARY KEY (`uid`),
  UNIQUE KEY `uk_user_name` (`name`),
  UNIQUE KEY `uk_user_email` (`email`)
) ENGINE=InnoDB AUTO_INCREMENT=1014 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `user`
--

LOCK TABLES `user` WRITE;
/*!40000 ALTER TABLE `user` DISABLE KEYS */;
INSERT INTO `user` VALUES (1001,'西故','3099154616@qq.com','$argon2id$v=19$m=262144,t=3,p=1$bD899q54K8vMwy+5fc7JKA$/jVJP8RyQj0mFzJqY/56T2F9f3SB74NSBm9Lxc0XoZ8','xigu','',0,''),(1002,'霄宫','xigu920@163.com','$argon2id$v=19$m=262144,t=3,p=1$o/CzQVrUYHFP1kQbYhSztw$7KdjCxJlu0I7uDrmqqIivmuBUfII15hYy66h3Q6H870','烟花','',1,''),(1003,'test1','123@123.com','$argon2id$v=19$m=65536,t=2,p=1$tXdYqduAQZKxtsyo3ac+rQ$voCSwFb/G2QfrcmexfKS8KLxyTaDS3TWC3bmVmzy6WU','','',0,''),(1004,'test2','456@456.com','$argon2id$v=19$m=65536,t=2,p=1$gNlpRDd7H5TFIwsvI/uZVQ$sh+mt+qXh0xQ0fA5Ij1E75KRnWrUVl4MMlFihDbccUs','','',0,''),(1005,'test3','789@789.com','$argon2id$v=19$m=65536,t=2,p=1$8B35hu43rJDUBr0yoqwExA$5HzrZBveaYnAXzXgcKc1d+lixv6vkbvVcWmxOi2F65Q','','',0,'');
/*!40000 ALTER TABLE `user` ENABLE KEYS */;
UNLOCK TABLES;

--
-- Dumping routines for database 'xigu'
--
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

-- Dump completed on 2026-11-19  6:49:27
