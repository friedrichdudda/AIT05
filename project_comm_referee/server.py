import logging
import asyncio
import aiocoap.resource as resource
import aiocoap


class Player:
    host: str = ""
    count: int = 0

    def __init__(self, host: str):
        self.host = host


registered_players: list[Player] = []


class IncrementPlayerCount(resource.Resource):
    async def render_put(self, request):
        print("Increment player count: %s" % request.remote.hostinfo)

        player = next(
            x for x in registered_players if x.host == request.remote.hostinfo
        )

        player.count += 1

        return aiocoap.Message(
            code=aiocoap.Code.CHANGED,
            payload="\n".join([request.remote.hostinfo, str(player.count)]).encode(
                "utf8"
            ),
        )


# logging setup

logging.basicConfig(level=logging.INFO)
logging.getLogger("coap-server").setLevel(logging.DEBUG)


async def discover_dictionary():
    protocol = await aiocoap.Context.create_client_context()

    request = aiocoap.Message(
        mtype=aiocoap.Type.NON,
        code=aiocoap.Code.GET,
        uri="coap://[ff02::1]/.well-known/core?rt=core.rd*",
    )

    try:
        response = await protocol.request(request).response
    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
        return e
    else:
        return str(response.remote.hostinfo)


async def discover_players(rd_address):
    protocol = await aiocoap.Context.create_client_context()

    request = aiocoap.Message(
        code=aiocoap.Code.GET,
        uri=f"coap://{rd_address}/resource-lookup/?rt=pushups_player",
    )

    try:
        response = await protocol.request(request).response

    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
    else:
        print("Result: %s\n%r" % (response.code, response.payload))


async def start_server():
    # Resource tree creation
    root = resource.Site()

    root.add_resource(
        [".well-known", "core"], resource.WKCResource(root.get_resources_as_linkheader)
    )
    root.add_resource(["increment_player_count"], IncrementPlayerCount())

    await aiocoap.Context.create_server_context(root)

    # Run forever
    await asyncio.get_running_loop().create_future()


async def main():
    # Discover resource dictionary via multicast
    resource_directory_ip_address = await discover_dictionary()

    # Discover players in resource directory
    await discover_players(resource_directory_ip_address)

    # Start server to listen for client events
    # await start_server()


if __name__ == "__main__":
    asyncio.run(main())
