<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Copy every node that no template matches. -->
  <xsl:mode on-no-match="shallow-copy"/>

  <!-- Editorial notes and the comment at the top are for the authors. -->
  <xsl:template match="cref | /comment()"/>

  <!-- An IANA section without actions is removed before publication. -->
  <xsl:template match="section[@anchor = 'iana']
                              [normalize-space(t) = 'This document has no IANA actions.']"/>
</xsl:stylesheet>
