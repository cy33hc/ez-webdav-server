# ez-webdav-server

This is a customized WebDAV Server for the PS4/PS5. It implements Class 1 and fakes Class 2 capibilities so that it can be mounted on Windows as a network drive.

<img width="1149" height="717" alt="{1DADD0EB-FDE8-4FF0-9696-43CB09988769}" src="https://github.com/user-attachments/assets/9db4b156-058a-4cdb-ba2d-8ac20a7f4ec1" />

## Why implement a WebDAV Server?
 - On Windows/Linux/MacOS you can mount WebDAV Server onto the laptop/desktop and directly work with the files on the PS4/PS5 as if they are local on the laptop/desktop.

## How to Start WebDAV
 - On PS5, send the webdav-server-ps5.elf to the elfloader on port 9021
 - On PS4, send the webdav-server-ps4.elf to Goldhen binloader

You should see a message on the screen "WebDAV Server Starting XXX on port 8880"

## How to Stop WebDAV
 - Execute `curl http://<PS5_IP>:8880/stop` or paste the URL into the firefox/Chrome address bar
 - You should see the message "WebDAV Server Stopped" on the PS5/PS4

## How to mount the WebDAV server on the laptop/desktop

### Windows
 - Windows Explorer has native support for WebDAV. You do that via the "map a networ drive" function. Found this link to be pretty helpful. https://servicecenter.fsu.edu/s/article/How-do-I-use-WebDAV-with-Windows-11-Individual <br/>
   **WARNING:** The windows builtin functions for webdav is not good for very large files. It's got a limit of 50MB only and a max upload timeout of 60sec. You can change these settings, but I found even the max isn't good for very large files. Also is uses caching, that means the file operations are not directly synced to the PS4/PS5. The operation will appear to have completed, but actually there is a background process that sync the cache to the PS4/PS5, so you might think the file has already completed upload when it hasn't

 - RClone **(Recommeded)**<br/>
   Rclone can do the same thing as Windows Explorer, where it can mount the WebDAV Server as a Network Drive.

   - First create a repo config for WebDAV.
     - Execute `rclone.exe config`
       1. Select "n) New remote"
       2. Give the remote a custom name. example "PS5"
       3. Select "63 / WebDAV"
       4. Enter the URL. Example: "http://<PS5_IP>:8880"
       5. Select "3 / Owncloud 10 PHP based WebDAV server". **MUST SELECT 3. I've found this is the only setup that doesn't use caching***
       6. Leave the username, password, bearer_token  Empty
       7. Select "No" for adavanced config
       8. Select "Yes" to save
       9. Select "q) Quit config"

   - Run the following command to mount the network drive. In my example, I've choose the "Z:" drive
     - Create a mount_ps5.bat file and put the following into the file<br/>
       `rclone mount PS5:/ Z: --network-mode --vfs-cache-mode off --dir-cache-time 5s`
     - Execute the mount_ps5.bat. If successfull, you should see the "Z:" drive mounted

### Linux
  - Open the file browser and enter `dav://<PS5_IP>:8880` in the address bar

### MacOS
  - I don't own any Applie products so I don't know how to do it. But here is a link https://support.apple.com/en-ca/guide/mac-help/mchlp1546/mac
