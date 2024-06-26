#include <aws/core/Aws.h>
#include <aws/core/client/AWSClientAsyncCRTP.h>
#include <aws/s3/S3Client.h>
#include <aws/sts/STSClient.h>
#include <aws/s3control/S3ControlClient.h>
#include <iostream>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>
#include <aws/s3control/model/PublicAccessBlockConfiguration.h>
#include <aws/s3control/model/PutPublicAccessBlockRequest.h>
#include <aws/s3control/model/GetPublicAccessBlockRequest.h>
#include <aws/sts/model/AssumeRoleRequest.h>
#include <future>
#include <thread>

using namespace Aws;
using namespace Aws::STS::Model;
using namespace Aws::S3Control::Model;
using namespace Aws::Auth;

/*
 *
 * Application run in cluster(kops) on AWS as a cron job to;
 * retrieve pod permissions to perform s3 control actions
 * turn off Account public ACL Block configuration that automatically turns on
 * allowing kops cluster access to oidc as well as data and config-store s3 public buckets
 *
 */

// helper fucntion for logging current block config settings
void printBlockConfig(const Aws::S3Control::Model::PublicAccessBlockConfiguration&);
// Async call func
void GetObjectAsyncFinished(const Aws::S3Control::S3ControlClient *s3Client,
                            const Aws::S3Control::Model::GetPublicAccessBlockRequest &request,
                            const Aws::S3Control::Model::GetPublicAccessBlockOutcome &outcome,
                            const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context);
// condition variable for async  done
std::condition_variable unblock_notify;
std::mutex lock;

int main(int argc, char **argv) {
    /*
     * TODO: take coomand line flags for acct and kops bucket name
     * For security reasons, program does not take any input at the moment 
    */
    // if (argc > 1) {
    //     return 1;
    // }

    Aws::SDKOptions options;
    // Optionally change the log level for debugging.
    options.loggingOptions.logLevel = Utils::Logging::LogLevel::Trace;
    
    Aws::InitAPI(options); // Once.
    int exit = 1;
    std::unique_lock<std::mutex> unique_lock(lock);
    {
        
        Aws::Client::ClientConfiguration clientConfig;
        // Optional: Set to the AWS Region (overrides config file).
        // clientConfig.region = "us-east-1";
        
        // role session name required for Kubernetes ServiceAccount WebIdentity Auth. Test
        auto setRoleName = AssumeRoleRequest();
        if (setRoleName.RoleSessionNameHasBeenSet() == true){
            std::cout << "AWS_ROLE_SESSION_NAME is set" << std::endl;
        } else {
            setRoleName.SetRoleSessionName("kops_s3_acl");
            std::cout << "setting AWS_ROLE_SESSION_NAME: kops_s3_acl" << std::endl;
        }

        // Test Cred Chain Delegation success
        try{
            auto provider = Aws::MakeShared<DefaultAWSCredentialsProviderChain>("alloc-tag");
            auto creds = provider->GetAWSCredentials();
            if (creds.IsEmpty()) {
                std::cerr << "\nFailed authentication... kops_s3_acl bailing out..." << std::endl;

                return exit;
            }
        }catch(std::exception){
            std::cout << "Last Cred Chain: WebIdentityProvider failed. Exiting..." << std::endl;
            
            return exit;
        }

        Aws::S3::S3Client s3Client(clientConfig);
        auto outcome = s3Client.ListBuckets();

        if (!outcome.IsSuccess()) {
            std::cerr << "\nFailed with error: " << outcome.GetError() << std::endl;
            
        } else {
            std::cout << "\nS3 Bucket Access: Discovered " << outcome.GetResult().GetBuckets().size()
                      << " buckets\n";
            // for (auto &bucket: outcome.GetResult().GetBuckets()) {
                // std::cout << bucket.GetName() << std::endl;
            // }
        }

        if (outcome.GetResult().GetBuckets().size() == 0) {
            std::cout << "\nNo S3 Bucket for Kops oidc or state store found in this AWS account. No Acct ACL block changes made. Exiting..." << std::endl;
            
            return exit;
        }

        Aws::STS::STSClient stsClient(clientConfig);
        auto req = GetCallerIdentityRequest();
        auto dataOut = stsClient.GetCallerIdentity(req);
        // auto acctId = data;
        // std::cout << "\nAcct_ID is : \n" << acctId << std::endl;
        if (!dataOut.IsSuccess()) {
            std::cerr << "\nFailed with error: " << dataOut.GetError() << std::endl;
            
        } else {
            std::cout << "\nAWS Acct ID endin in: " << "XXXXXX"<< dataOut.GetResult().GetAccount().substr(7) << " retrieved"<< std::endl;
        }
        auto acctId = dataOut.GetResult().GetAccount();

        Aws::S3Control::S3ControlClient s3Cont(clientConfig);
        const auto getReq = GetPublicAccessBlockRequest().WithAccountId(acctId);
        auto aclBlock = s3Cont.GetPublicAccessBlock(getReq);
        Aws::S3Control::Model::PublicAccessBlockConfiguration block;
        
        if (!aclBlock.IsSuccess()) {
            std::cout << "\nFailed with error : " << aclBlock.GetError() << std::endl;
            
        } else {
            block = aclBlock.GetResult().GetPublicAccessBlockConfiguration();
            printBlockConfig(block);
        }

        /*
        * this outer if block is ensuring the s3 acl turns on before the code turns it off - efficcient
        * but the JWKs public endpoint > https://<idp>/well-known/opend-configuration < is unreachable when s3 public access is blocked
        * if this code runs in kops cluster with s3 issuer discovery, the s3 public access mustn't turn back on
        */
        // if (block.GetBlockPublicAcls() && block.GetIgnorePublicAcls() && block.GetBlockPublicPolicy() && block.GetRestrictPublicBuckets()) {
                    
        //turn acct ACL Block config off for kops s3 access if on
        block.SetBlockPublicAcls(false);
        block.SetIgnorePublicAcls(false);
        block.SetBlockPublicPolicy(false);
        block.SetRestrictPublicBuckets(false);
        
        auto putReq = PutPublicAccessBlockRequest().WithAccountId(acctId).WithPublicAccessBlockConfiguration(block);
        auto putBlock = s3Cont.PutPublicAccessBlock(putReq);

        if (!putBlock.IsSuccess()) {
            std::cout << "\nFailed with error : " << putBlock.GetError() << std::endl;
            
        } else {

            std::shared_ptr<const Aws::Client::AsyncCallerContext> context = Aws::MakeShared<Aws::Client::AsyncCallerContext>("PutAllocationTag");
            s3Cont.GetPublicAccessBlockAsync(getReq, GetObjectAsyncFinished, context);
            unblock_notify.wait(unique_lock);
            std::cout << "Relaxed Block ACL Finished" << std::endl;

        // std::cout << "Done" << std::endl;
        // auto fut = Aws::MakeShared< std::packaged_task< Aws::S3Control::Model::GetPublicAccessBlockOutcome()>>("alloc-tag", futBlock)->get_future();
        
        // futBlock->*get_future();
        // Aws::Client::AsyncCallerContext::S
        // auto fut = Aws::MakeShared< std::packaged_task< Aws::S3Control::Model::GetPublicAccessBlockOutcomeCallable()>>("alloc-tag", futBlock);
        // auto task = fut->get_future();
        // std::thread t(std::move(&task));
        // task.wait();
        // t.join();
        // auto task = fut->get_future();
        // std::thread t(std::move(&task));
        // std::cout << task.get().GetResult().GetPublicAccessBlockConfiguration().GetBlockPublicAcls() << std::endl;
        
        // std::packaged_task< Aws::S3Control::Model::GetPublicAccessBlockOutcome()> task();
        // fut->get_future();
        // std::shared_ptr<Aws::Client::AsyncCallerContext> context = Aws::MakeShared<Aws::Client::AsyncCallerContext>("alloc-tag", futBlock);
        // context->SetUUID(acctId);

        // block = GetResult().GetPublicAccessBlockConfiguration();
        // printBlockConfig(block);

        }
        // }
    }

    Aws::ShutdownAPI(options); // Should only be called once.
    return 0;
}

void printBlockConfig(const Aws::S3Control::Model::PublicAccessBlockConfiguration &b){
            bool blockPubAcl, ignorePubAcl, blockPubPol, restrctPubBuk;
            blockPubAcl = b.GetBlockPublicAcls();
            ignorePubAcl = b.GetIgnorePublicAcls();
            blockPubPol = b.GetBlockPublicPolicy();
            restrctPubBuk = b.GetRestrictPublicBuckets();
        
            std::cout << "\nBlockPublicACL is : " << (blockPubAcl? "on" : "off") << "\n"
                        "IgnorePublicACL is : " << (ignorePubAcl? "on" : "off") << "\n"
                        "BlockPublicPolicy is : " << (blockPubPol? "on" : "off") << "\n"
                        "RestrictPublicBucket is : " << (restrctPubBuk? "on" : "off") << std::endl;

}

void GetObjectAsyncFinished(const Aws::S3Control::S3ControlClient *s3Client,
                            const Aws::S3Control::Model::GetPublicAccessBlockRequest &request,
                            const Aws::S3Control::Model::GetPublicAccessBlockOutcome &outcome,
                            const std::shared_ptr<const Aws::Client::AsyncCallerContext> &context) {

    if (!outcome.IsSuccess()) {
        std::cout << "\nFailed with error : " << outcome.GetError().GetMessage() << std::endl;
    } else {
        std::cout << "Unblock ACL Configuration For KOPs Cluster" << std::endl;
        auto block = outcome.GetResult().GetPublicAccessBlockConfiguration();
        printBlockConfig(block);
    }

    unblock_notify.notify_one();
}