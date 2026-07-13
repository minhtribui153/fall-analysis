# fall-analysis
An NYP Data Engineering Project that checks for fall detection.

> To ensure project runs smoothly, please ensure that you have
> the following tools and programming languages installed on
> your system:
> - GNU Make
> - Arduino
> - Docker
> - Python
> - Node.JS
> - C++

> This project assumes that you are inside a Raspberry Pi 5
> Linux environment for any server deployment, which would
> already have most of the tools listed above installed
> by default. If inside, you will only need to install
> Docker and Node.JS.

### Deploying Docker Containers
> Please ensure that you have created a `.env` file into the
> project root directory, with reference from `.env.example`
> before continuing.

> Make sure to also run `sudo make init` first so that
> the project will automatically set up the proper
> permissions, particularly the databases.

```bash
# Note that <container_name> refers to compose.<container_name>.yaml
# To deploy a container instance
sudo make deploy up <container_name>

# To stop a container instance
sudo make deploy down <container_name>
```