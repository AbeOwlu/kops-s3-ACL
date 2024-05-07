## KOPs-S3-ACL

With the intorduction of block access configuration, https://docs.aws.amazon.com/AmazonS3/latest/userguide/access-control-block-public-access.html#configuring-block-public-access, overriding individual buckes acl and policy configuration.

And KOPs cluster on AWS requirement https://kops.sigs.k8s.io/getting_started/aws/#cluster-oidc-store, to save cluster state and oidc in S3 bucket(s) configured, public ACL access is required.

This program checks AWS account s3 ACL configuration and turns it off to alows KOPs to s3 access

---

### Build and test
Clone this repo
```
git clone https://github.com/AbeOwlu/kops-s3-ACL.git
```

Application code (main.cpp) can be reviewed, modified, built and tested locally;
```
cd src/ #open main.cpp in editor
mkdir ../build && cd build && cmake ../src --build . --config=Debug
```

A ready container image that can be deployed as a runtime container (batch/job) on a kops cluster is available here public.ecr.aws/f6q5p7u8/kops-s3-acl:latest, for linux/amd64. Docker image for other architectures can be built using the dockerfile available in this repo;
```
docker build --target build-app  --platform <linux/amd64> -t <"$IMAGE_TAG"> .
```

An example deployment is avalailable in the [exampe usage folder](). The block ACL change is run every 1.5 hours as a cron job.


AAAAAAAAAaaaaaaaA  

    dfdf