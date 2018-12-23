function [ofs,gain,rotM,u]=ellipsoid_fit(XYZ, rotate_fit)
% Fit a rotated ellipsoid to a set of xyz data points
% XYZ: N(rows) x 3(cols), matrix of N data points (x,y,z)

x=XYZ(:,1); y=XYZ(:,2); z=XYZ(:,3);
x2=x.*x; y2=y.*y; z2=z.*z;
    
if rotate_fit == 1
    D = [x2+y2-2*z2, x2-2*y2+z2, 4*x.*y, 2*x.*z, 2*y.*z, 2*x, 2*y, 2*z, ones(length(x),1)];
    R = x2+y2+z2;
    b = (D'*D)\(D'*R); % least square solution
    mtxref = [ 3 1 1 0 0 0 0 0 0 0; 3 1 -2 0 0 0 0 0 0 0; 3 -2 1 0 0 0 0 0 0 0; ...
    0 0 0 2 0 0 0 0 0 0; 0 0 0 0 1 0 0 0 0 0; 0 0 0 0 0 1 0 0 0 0; ...
    0 0 0 0 0 0 1 0 0 0; 0 0 0 0 0 0 0 1 0 0; 0 0 0 0 0 0 0 0 1 0; ...
    0 0 0 0 0 0 0 0 0 1];
    v = mtxref*[-1/3; b]; u=v; nn=v(10); v = -v(1:9);
    A = [ v(1) v(4) v(5) v(7); v(4) v(2) v(6) v(8); v(5) v(6) v(3) v(9); v(7) v(8) v(9) -nn ];
    ofs=-A(1:3,1:3)\[v(7);v(8);v(9)]; % offset is center of ellipsoid
    Tmtx=eye(4); Tmtx(4,1:3)=ofs'; AT=Tmtx*A*Tmtx'; % ellipsoid translated to (0,0,0)
    [rotM ev]=eig(AT(1:3,1:3)/-AT(4,4)); % eigenvectors (rotation) and eigenvalues (gain)
    gain=sqrt(1./diag(ev)); % gain is radius of the ellipsoid
else
    D=[x2, y2, z2, 2*x,2*y,2*z]; % XYZ axes (non-rotated ellipsoid)
    v = (D'*D)\(D'*ones(length(x),1)); % least square fitting
    v = [ v(1) v(2) v(3) 0 0 0 v(4) v(5) v(6) ]; u=v;
    ofs=-(v(1:3).\v(7:9))'; % offset is center of ellipsoi
    rotM=eye(3); % eigenvectors (rotation), identity = no rotation 
    g=1+(v(7)^2/v(1)+v(8)^2/v(2)+v(9)^2/v(3));
    gain=(sqrt(g./v(1:3)))'; % find radii of the ellipsoid (scale
end